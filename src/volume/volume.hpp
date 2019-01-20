#pragma once

#include "device/device.h"
#include <fcntl.h>
#include <cache/cache_common.hpp>
#include <cache/cache.h>
#include "blkstore/writeBack_cache.hpp"
#include <device/blkbuffer.hpp>
#include <blkstore/blkstore.hpp>
#include "home_blks.hpp"
#include <metrics/metrics.hpp>
#include <utility/atomic_counter.hpp>

#include "threadpool/thread_pool.h"
using namespace std;

#ifndef NDEBUG
extern std::atomic< int > vol_req_alloc;
#endif
namespace homestore {

class mapping;
enum vol_state;

struct Free_Blk_Entry {
    BlkId   m_blkId;
    uint8_t m_blk_offset : NBLKS_BITS;
    uint8_t m_nblks_to_free : NBLKS_BITS;

    Free_Blk_Entry(const BlkId& m_blkId, uint8_t m_blk_offset, uint8_t m_nblks_to_free) :
            m_blkId(m_blkId),
            m_blk_offset(m_blk_offset),
            m_nblks_to_free(m_nblks_to_free) {}
};

struct volume_req : blkstore_req< BlkBuffer > {
    uint64_t                      lba;
    int                           nlbas;
    bool                          is_read;
    std::shared_ptr< Volume >     vol_instance;
    std::vector< Free_Blk_Entry > blkIds_to_free;
    uint64_t                      seqId;
    uint64_t                      lastCommited_seqId;

    /* number of times mapping table need to be updated for this req. It can
     * break the ios update in mapping btree depending on the key range.
     */
    std::atomic< int >                        num_mapping_update;
    boost::intrusive_ptr< vol_interface_req > parent_req;
    bool                                      done;

public:
    volume_req() : is_read(false), num_mapping_update(0), parent_req(nullptr), done(false) {
#ifndef NDEBUG
        vol_req_alloc++;
#endif
    }

    /* any derived class should have the virtual destructor to prevent
     * memory leak because pointer can be free with the base class.
     */
    virtual ~volume_req() {
#ifndef NDEBUG
        vol_req_alloc--;
#endif
    }
};

class VolumeMetrics : public sisl::MetricsGroupWrapper {
public:
    explicit VolumeMetrics(const char* vol_name) : sisl::MetricsGroupWrapper(vol_name) {
        REGISTER_COUNTER(volume_read_count, "Total Volume read operations");
        REGISTER_COUNTER(volume_write_count, "Total Volume write operations");
        REGISTER_COUNTER(volume_read_error_count, "Total Volume read error count");
        REGISTER_COUNTER(volume_write_error_count, "Total Volume write error count");
        REGISTER_COUNTER(volume_write_error_count, "Total Volume write error count");

        REGISTER_HISTOGRAM(volume_read_latency, "Volume overall read latency");
        REGISTER_HISTOGRAM(volume_write_latency, "Volume overall write latency");
        REGISTER_HISTOGRAM(volume_data_read_latency, "Volume data blocks read latency");
        REGISTER_HISTOGRAM(volume_data_write_latency, "Volume data blocks write latency");
        REGISTER_HISTOGRAM(volume_map_read_latency, "Volume mapping read latency");
        REGISTER_HISTOGRAM(volume_map_write_latency, "Volume mapping write latency");
        REGISTER_HISTOGRAM(volume_blkalloc_latency, "Volume block allocation latency");
        REGISTER_HISTOGRAM(volume_pieces_per_write, "Number of individual pieces per write",
                           sisl::HistogramBucketsType(LinearUpto64Buckets));

        register_me_to_farm();
    }
};

class Volume : public std::enabled_shared_from_this< Volume > {
private:
    mapping*                          m_map;
    boost::intrusive_ptr< BlkBuffer > m_only_in_mem_buff;
    struct vol_sb*                    m_sb;
    enum vol_state                    m_state;
    void                              alloc_single_block_in_mem();
    void                              vol_scan_alloc_blks();
    io_comp_callback                  m_comp_cb;
    std::atomic< uint64_t >           seq_Id;
    VolumeMetrics                     m_metrics;

private:
    Volume(vol_params& params);
    Volume(vol_sb* sb);

public:
    template < typename... Args >
    static std::shared_ptr< Volume > make_volume(Args&&... args) {
        return std::shared_ptr< Volume >(new Volume(std::forward< Args >(args)...));
    }

    static homestore::BlkStore< homestore::VdevVarSizeBlkAllocatorPolicy >* m_data_blkstore;
    static void process_vol_data_completions(boost::intrusive_ptr< blkstore_req< BlkBuffer > > bs_req);

    ~Volume() { free(m_sb); };
    std::error_condition destroy();
    void                 process_metadata_completions(boost::intrusive_ptr< volume_req > wb_req);
    void                 process_data_completions(boost::intrusive_ptr< blkstore_req< BlkBuffer > > bs_req);

    std::error_condition write(uint64_t lba, uint8_t* buf, uint32_t nblks,
                               boost::intrusive_ptr< vol_interface_req > req);

    std::error_condition read(uint64_t lba, int nblks, boost::intrusive_ptr< vol_interface_req > req, bool sync);

    void process_metadata_completions(boost::intrusive_ptr< volume_req > req);
    void process_data_completions(boost::intrusive_ptr< blkstore_req< BlkBuffer > > bs_req);

    uint64_t get_elapsed_time(Clock::time_point startTime);
    void     attach_completion_cb(io_comp_callback& cb);

    void                      print_tree();

    void blk_recovery_process_completions(bool success);
    void blk_recovery_callback(MappingValue& mv);

    mapping* get_mapping_handle() { return m_map; }

    uint64_t get_last_lba() {
        assert(m_sb->size != 0);
        // lba starts from 0, then 1, 2, ...
        if (m_sb->size % HomeStoreConfig::phys_page_size == 0)
            return m_sb->size / HomeStoreConfig::phys_page_size - 1;
        else
            return m_sb->size / HomeStoreConfig::phys_page_size;
    }

    vol_sb* get_sb() { return m_sb; };

    const char* get_name() const { return (m_sb->vol_name); }
    uint64_t    get_page_size() const { return m_sb->page_size; }
    uint64_t    get_size() const { return m_sb->size; }

#ifndef NDEBUG
    void enable_split_merge_crash_simulation();
#endif
};

#define BLKSTORE_BLK_SIZE_IN_BYTES HomeStoreConfig::phys_page_size
#define QUERY_RANGE_IN_BYTES (64 * 1024 * 1024ull)
#define NUM_BLKS_PER_THREAD_TO_QUERY (QUERY_RANGE_IN_BYTES / BLKSTORE_BLK_SIZE_IN_BYTES)

class BlkAllocBitmapBuilder {
    typedef std::function< void(MappingValue& mv) > blk_recovery_callback;
    typedef std::function< void(bool success) >     comp_callback;

private:
    homestore::Volume*    m_vol_handle;
    blk_recovery_callback m_blk_recovery_cb;
    comp_callback         m_comp_cb;

public:
    BlkAllocBitmapBuilder(homestore::Volume* vol, blk_recovery_callback blk_rec_cb, comp_callback comp_cb) :
            m_vol_handle(vol),
            m_blk_recovery_cb(blk_rec_cb),
            m_comp_cb(comp_cb) {}
    ~BlkAllocBitmapBuilder();

    // async call to start the multi-threaded work.
    void get_allocated_blks();

private:
    // do the real work of getting all allocated blks in multi-threaded manner
    void do_work();
};

} // namespace homestore
