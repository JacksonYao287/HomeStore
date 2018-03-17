/*
 * ssd_btree.hpp
 *
 *  Created on: 14-Jun-2016
 *      Author: Hari Kadayam
 *
 *  Copyright © 2016 Kadayam, Hari. All rights reserved.
 */
#pragma once

#include <iostream>
#include <assert.h>
#include <pthread.h>
#include <vector>
#include <atomic>

#include "omds/memory/composite_allocator.hpp"
#include "omds/memory/chunk_allocator.hpp"
#include "omds/memory/sys_allocator.hpp"
#include "cache.h"
#include "blkstore/blkstore.hpp"
#include "omds/btree/btree_specific_impl.hpp"
#include "omds/btree/btree_node.h"
#include "omds/btree/physical_node.hpp"

using namespace omstore;

namespace omds { namespace btree {

#define SSDBtreeNodeDeclType BtreeNode<SSD_BTREE, K, V, InteriorNodeType, LeafNodeType, NodeSize>
#define SSDBtreeImpl BtreeSpecificImpl<SSD_BTREE, K, V, InteriorNodeType, LeafNodeType, NodeSize>
#define BtreeBufferDeclType BtreeBuffer<K, V, InteriorNodeType, LeafNodeType, NodeSize>

/* The class BtreeBuffer represents the buffer type that is used to interact with the BlkStore. It will have
 * all the SSD Btree Node declarative type. Hence in-memory representation of this buffer is as follows
 *
 *   ****************Cache Buffer************************
 *   *    ****************Cache Record***************   *
 *   *    *   ************Hash Node**************   *   *
 *   *    *   * Singly Linked list of hash node *   *   *
 *   *    *   ***********************************   *   *
 *   *    *******************************************   *
 *   * BlkId                                            *
 *   * Memvector of actual buffer                       *
 *   * Usage Reference counter                          *
 *   ****************************************************
 *   ************** Transient Header ********************
 *   * Upgraders count                                  *
 *   * Reader Write Lock                                *
 *   ****************************************************
 */
template<
        typename K,
        typename V,
        btree_node_type InteriorNodeType,
        btree_node_type LeafNodeType,
        size_t NodeSize
>
class BtreeBuffer : public CacheBuffer< BlkId > {
public:
    static BtreeBuffer *make_object() {
        return omds::ObjectAllocator< SSDBtreeNodeDeclType >::make_object();
    }
};

struct btree_device_info {
    DeviceManager *dev_mgr;
    Cache< omstore::BlkId > *cache;
    vdev_info_block *vb;
    uint64_t size;
    bool new_device;
};

template<
        typename K,
        typename V,
        btree_node_type InteriorNodeType,
        btree_node_type LeafNodeType,
        size_t NodeSize
>
class omds::btree::BtreeSpecificImpl<SSD_BTREE, K, V, InteriorNodeType, LeafNodeType, NodeSize> {
public:
    using HeaderType = BtreeBuffer<K, V, InteriorNodeType, LeafNodeType, NodeSize>;

    static std::unique_ptr<SSDBtreeImpl> init_btree(BtreeConfig &cfg, void *btree_specific_context) {
        return std::make_unique<SSDBtreeImpl>(btree_specific_context);
    }

    BtreeSpecificImpl(void *btree_specific_context) {
        auto bt_dev_info = (btree_device_info *)btree_specific_context;

        // Create or load the Blkstore out of this info
        if (bt_dev_info->new_device) {
            m_blkstore = new BlkStore<VdevFixedBlkAllocatorPolicy, BtreeBufferDeclType>(
                    bt_dev_info->dev_mgr, bt_dev_info->cache, bt_dev_info->size, BlkStoreCacheType::WRITETHRU_CACHE, 0);
        } else {
            m_blkstore = new BlkStore<VdevFixedBlkAllocatorPolicy, BtreeBufferDeclType>
                    (bt_dev_info->dev_mgr, bt_dev_info->cache, bt_dev_info->vb, BlkStoreCacheType::WRITETHRU_CACHE);
        }
    }

    static uint8_t *get_physical(const SSDBtreeNodeDeclType *bn) {
        BtreeBufferDeclType *bbuf = (BtreeBufferDeclType *)(bn);
        omds::blob b = bbuf->at_offset(0);
        assert(b.size == NodeSize);
        return b.bytes;
    }

    static uint32_t get_node_area_size(SSDBtreeImpl *impl) {
        return NodeSize - sizeof(SSDBtreeNodeDeclType) - sizeof(LeafPhysicalNodeDeclType);
    }

    static boost::intrusive_ptr<SSDBtreeNodeDeclType> alloc_node(SSDBtreeImpl *impl, bool is_leaf) {
        blk_alloc_hints hints;
        BlkId blkid;
        auto safe_buf = impl->m_blkstore->alloc_blk_cached(1, hints, &blkid);

        // Access the physical node buffer and initialize it
        omds::blob b = safe_buf->at_offset(0);
        assert(b.size == NodeSize);
        if (is_leaf) {
            auto n = new (b.bytes) VariantNode<LeafNodeType, K, V, NodeSize>((bnodeid_t)blkid.get_id(), true);
        } else {
            auto n = new (b.bytes) VariantNode<InteriorNodeType, K, V, NodeSize>((bnodeid_t)blkid.get_id(), true);
        }

        return boost::static_pointer_cast<SSDBtreeNodeDeclType>(safe_buf);
    }

    static boost::intrusive_ptr<SSDBtreeNodeDeclType> read_node(SSDBtreeImpl *impl, bnodeid_t id) {
        // Read the data from the block store
        BlkId blkid(id.to_integer());
        auto safe_buf = impl->m_blkstore->read(blkid, 0, NodeSize);

        return boost::static_pointer_cast<SSDBtreeNodeDeclType>(safe_buf);
    }

    static void write_node(SSDBtreeImpl *impl, boost::intrusive_ptr<SSDBtreeNodeDeclType> bn) {
        BlkId blkid(bn->get_node_id().to_integer());
        impl->m_blkstore->write(blkid, boost::dynamic_pointer_cast<BtreeBufferDeclType>(bn));
    }

    static void free_node(SSDBtreeImpl *impl, boost::intrusive_ptr<SSDBtreeNodeDeclType> bn) {
        BlkId blkid(bn->get_node_id().to_integer());
        impl->m_blkstore->free_blk(blkid, boost::none, boost::none);
    }

    static void ref_node(SSDBtreeNodeDeclType *bn) {
        CacheBuffer< BlkId >::ref((CacheBuffer<BlkId> &)*bn);
    }

    static bool deref_node(SSDBtreeNodeDeclType *bn) {
        return CacheBuffer< BlkId >::deref_testz((CacheBuffer<BlkId> &)*bn);
    }
private:
    BlkStore<VdevFixedBlkAllocatorPolicy, BtreeBufferDeclType> *m_blkstore;
};
} }


