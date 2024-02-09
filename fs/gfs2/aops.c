/*
 * Copyright (C) Sistina Software, Inc.  1997-2003 All rights reserved.
 * Copyright (C) 2004-2008 Red Hat, Inc.  All rights reserved.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License version 2.
 */

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/buffer_head.h>
#include <linux/pagemap.h>
#include <linux/pagevec.h>
#include <linux/mpage.h>
#include <linux/fs.h>
#include <linux/writeback.h>
#include <linux/swap.h>
#include <linux/gfs2_ondisk.h>
#include <linux/backing-dev.h>
#include <linux/uio.h>
#include <trace/events/writeback.h>

#include "gfs2.h"
#include "incore.h"
#include "bmap.h"
#include "glock.h"
#include "inode.h"
#include "log.h"
#include "meta_io.h"
#include "quota.h"
#include "trans.h"
#include "rgrp.h"
#include "super.h"
#include "util.h"
#include "glops.h"


static void gfs2_page_add_databufs(struct gfs2_inode *ip, struct page *page,
				   unsigned int from, unsigned int to)
{
	struct buffer_head *head = page_buffers(page);
	unsigned int bsize = head->b_size;
	struct buffer_head *bh;
	unsigned int start, end;

	for (bh = head, start = 0; bh != head || !start;
	     bh = bh->b_this_page, start = end) {
		end = start + bsize;
		if (end <= from || start >= to)
			continue;
		if (gfs2_is_jdata(ip))
			set_buffer_uptodate(bh);
		gfs2_trans_add_data(ip->i_gl, bh);
	}
}

/**
 * gfs2_get_block_noalloc - Fills in a buffer head with details about a block
 * @inode: The inode
 * @lblock: The block number to look up
 * @bh_result: The buffer head to return the result in
 * @create: Non-zero if we may add block to the file
 *
 * Returns: errno
 */

static int gfs2_get_block_noalloc(struct inode *inode, sector_t lblock,
				  struct buffer_head *bh_result, int create)
{
	int error;

	error = gfs2_block_map(inode, lblock, bh_result, 0);
	if (error)
		return error;
	if (!buffer_mapped(bh_result))
		return -EIO;
	return 0;
}

static int gfs2_get_block_direct(struct inode *inode, sector_t lblock,
				 struct buffer_head *bh_result, int create)
{
	return gfs2_block_map(inode, lblock, bh_result, 0);
}

/**
 * gfs2_writepage_common - Common bits of writepage
 * @page: The page to be written
 * @wbc: The writeback control
 *
 * Returns: 1 if writepage is ok, otherwise an error code or zero if no error.
 */

static int gfs2_writepage_common(struct page *page,
				 struct writeback_control *wbc)
{
	struct inode *inode = page->mapping->host;
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_sbd *sdp = GFS2_SB(inode);
	loff_t i_size = i_size_read(inode);
	pgoff_t end_index = i_size >> PAGE_SHIFT;
	unsigned offset;

	if (gfs2_assert_withdraw(sdp, gfs2_glock_is_held_excl(ip->i_gl)))
		goto out;
	if (current->journal_info)
		goto redirty;
	/* Is the page fully outside i_size? (truncate in progress) */
	offset = i_size & (PAGE_SIZE-1);
	if (page->index > end_index || (page->index == end_index && !offset)) {
		page->mapping->a_ops->invalidatepage(page, 0, PAGE_SIZE);
		goto out;
	}
	return 1;
redirty:
	redirty_page_for_writepage(wbc, page);
out:
	unlock_page(page);
	return 0;
}

/**
 * gfs2_writepage - Write page for writeback mappings
 * @page: The page
 * @wbc: The writeback control
 *
 */

static int gfs2_writepage(struct page *page, struct writeback_control *wbc)
{
	int ret;

	ret = gfs2_writepage_common(page, wbc);
	if (ret <= 0)
		return ret;

	return nobh_writepage(page, gfs2_get_block_noalloc, wbc);
}

/* This is the same as calling block_write_full_page, but it also
 * writes pages outside of i_size
 */
static int gfs2_write_full_page(struct page *page, get_block_t *get_block,
				struct writeback_control *wbc)
{
	struct inode * const inode = page->mapping->host;
	loff_t i_size = i_size_read(inode);
	const pgoff_t end_index = i_size >> PAGE_SHIFT;
	unsigned offset;

	/*
	 * The page straddles i_size.  It must be zeroed out on each and every
	 * writepage invocation because it may be mmapped.  "A file is mapped
	 * in multiples of the page size.  For a file that is not a multiple of
	 * the  page size, the remaining memory is zeroed when mapped, and
	 * writes to that region are not written out to the file."
	 */
	offset = i_size & (PAGE_SIZE-1);
	if (page->index == end_index && offset)
		zero_user_segment(page, offset, PAGE_SIZE);

	return __block_write_full_page(inode, page, get_block, wbc,
				       end_buffer_async_write);
}

/**
 * __gfs2_jdata_writepage - The core of jdata writepage
 * @page: The page to write
 * @wbc: The writeback control
 *
 * This is shared between writepage and writepages and implements the
 * core of the writepage operation. If a transaction is required then
 * PageChecked will have been set and the transaction will have
 * already been started before this is called.
 */

static int __gfs2_jdata_writepage(struct page *page, struct writeback_control *wbc)
{
	struct inode *inode = page->mapping->host;
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_sbd *sdp = GFS2_SB(inode);

	if (PageChecked(page)) {
		ClearPageChecked(page);
		if (!page_has_buffers(page)) {
			create_empty_buffers(page, inode->i_sb->s_blocksize,
					     BIT(BH_Dirty)|BIT(BH_Uptodate));
		}
		gfs2_page_add_databufs(ip, page, 0, sdp->sd_vfs->s_blocksize-1);
	}
	return gfs2_write_full_page(page, gfs2_get_block_noalloc, wbc);
}

/**
 * gfs2_jdata_writepage - Write complete page
 * @page: Page to write
 * @wbc: The writeback control
 *
 * Returns: errno
 *
 */

static int gfs2_jdata_writepage(struct page *page, struct writeback_control *wbc)
{
	struct inode *inode = page->mapping->host;
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_sbd *sdp = GFS2_SB(inode);
	int ret;

	if (gfs2_assert_withdraw(sdp, gfs2_glock_is_held_excl(ip->i_gl)))
		goto out;
	if (PageChecked(page) || current->journal_info)
		goto out_ignore;
	ret = __gfs2_jdata_writepage(page, wbc);
	return ret;

out_ignore:
	redirty_page_for_writepage(wbc, page);
out:
	unlock_page(page);
	return 0;
}

/**
 * gfs2_writepages - Write a bunch of dirty pages back to disk
 * @mapping: The mapping to write
 * @wbc: Write-back control
 *
 * Used for both ordered and writeback modes.
 */
static int gfs2_writepages(struct address_space *mapping,
			   struct writeback_control *wbc)
{
	struct gfs2_sbd *sdp = gfs2_mapping2sbd(mapping);
	int ret = mpage_writepages(mapping, wbc, gfs2_get_block_noalloc);

	/*
	 * Even if we didn't write any pages here, we might still be holding
	 * dirty pages in the ail. We forcibly flush the ail because we don't
	 * want balance_dirty_pages() to loop indefinitely trying to write out
	 * pages held in the ail that it can't find.
	 */
	if (ret == 0)
		set_bit(SDF_FORCE_AIL_FLUSH, &sdp->sd_flags);

	return ret;
}

/**
 * gfs2_write_jdata_pagevec - Write back a pagevec's worth of pages
 * @mapping: The mapping
 * @wbc: The writeback control
 * @pvec: The vector of pages
 * @nr_pages: The number of pages to write
 * @end: End position
 * @done_index: Page index
 *
 * Returns: non-zero if loop should terminate, zero otherwise
 */

static int gfs2_write_jdata_pagevec(struct address_space *mapping,
				    struct writeback_control *wbc,
				    struct pagevec *pvec,
				    int nr_pages, pgoff_t end,
				    pgoff_t *done_index)
{
	struct inode *inode = mapping->host;
	struct gfs2_sbd *sdp = GFS2_SB(inode);
	unsigned nrblocks = nr_pages * (PAGE_SIZE/inode->i_sb->s_blocksize);
	int i;
	int ret;

	ret = gfs2_trans_begin(sdp, nrblocks, nrblocks);
	if (ret < 0)
		return ret;

	for(i = 0; i < nr_pages; i++) {
		struct page *page = pvec->pages[i];

		/*
		 * At this point, the page may be truncated or
		 * invalidated (changing page->mapping to NULL), or
		 * even swizzled back from swapper_space to tmpfs file
		 * mapping. However, page->index will not change
		 * because we have a reference on the page.
		 */
		if (page->index > end) {
			/*
			 * can't be range_cyclic (1st pass) because
			 * end == -1 in that case.
			 */
			ret = 1;
			break;
		}

		*done_index = page->index;

		lock_page(page);

		if (unlikely(page->mapping != mapping)) {
continue_unlock:
			unlock_page(page);
			continue;
		}

		if (!PageDirty(page)) {
			/* someone wrote it for us */
			goto continue_unlock;
		}

		if (PageWriteback(page)) {
			if (wbc->sync_mode != WB_SYNC_NONE)
				wait_on_page_writeback(page);
			else
				goto continue_unlock;
		}

		BUG_ON(PageWriteback(page));
		if (!clear_page_dirty_for_io(page))
			goto continue_unlock;

		trace_wbc_writepage(wbc, inode_to_bdi(inode));

		ret = __gfs2_jdata_writepage(page, wbc);
		if (unlikely(ret)) {
			if (ret == AOP_WRITEPAGE_ACTIVATE) {
				unlock_page(page);
				ret = 0;
			} else {

				/*
				 * done_index is set past this page,
				 * so media errors will not choke
				 * background writeout for the entire
				 * file. This has consequences for
				 * range_cyclic semantics (ie. it may
				 * not be suitable for data integrity
				 * writeout).
				 */
				*done_index = page->index + 1;
				ret = 1;
				break;
			}
		}

		/*
		 * We stop writing back only if we are not doing
		 * integrity sync. In case of integrity sync we have to
		 * keep going until we have written all the pages
		 * we tagged for writeback prior to entering this loop.
		 */
		if (--wbc->nr_to_write <= 0 && wbc->sync_mode == WB_SYNC_NONE) {
			ret = 1;
			break;
		}

	}
	gfs2_trans_end(sdp);
	return ret;
}

/**
 * gfs2_write_cache_jdata - Like write_cache_pages but different
 * @mapping: The mapping to write
 * @wbc: The writeback control
 *
 * The reason that we use our own function here is that we need to
 * start transactions before we grab page locks. This allows us
 * to get the ordering right.
 */

static int gfs2_write_cache_jdata(struct address_space *mapping,
				  struct writeback_control *wbc)
{
	int ret = 0;
	int done = 0;
	struct pagevec pvec;
	int nr_pages;
	pgoff_t uninitialized_var(writeback_index);
	pgoff_t index;
	pgoff_t end;
	pgoff_t done_index;
	int cycled;
	int range_whole = 0;
	int tag;

	pagevec_init(&pvec, 0);
	if (wbc->range_cyclic) {
		writeback_index = mapping->writeback_index; /* prev offset */
		index = writeback_index;
		if (index == 0)
			cycled = 1;
		else
			cycled = 0;
		end = -1;
	} else {
		index = wbc->range_start >> PAGE_SHIFT;
		end = wbc->range_end >> PAGE_SHIFT;
		if (wbc->range_start == 0 && wbc->range_end == LLONG_MAX)
			range_whole = 1;
		cycled = 1; /* ignore range_cyclic tests */
	}
	if (wbc->sync_mode == WB_SYNC_ALL || wbc->tagged_writepages)
		tag = PAGECACHE_TAG_TOWRITE;
	else
		tag = PAGECACHE_TAG_DIRTY;

retry:
	if (wbc->sync_mode == WB_SYNC_ALL || wbc->tagged_writepages)
		tag_pages_for_writeback(mapping, index, end);
	done_index = index;
	while (!done && (index <= end)) {
		nr_pages = pagevec_lookup_tag(&pvec, mapping, &index, tag,
			      min(end - index, (pgoff_t)PAGEVEC_SIZE-1) + 1);
		if (nr_pages == 0)
			break;

		ret = gfs2_write_jdata_pagevec(mapping, wbc, &pvec, nr_pages, end, &done_index);
		if (ret)
			done = 1;
		if (ret > 0)
			ret = 0;
		pagevec_release(&pvec);
		cond_resched();
	}

	if (!cycled && !done) {
		/*
		 * range_cyclic:
		 * We hit the last page and there is more work to be done: wrap
		 * back to the start of the file
		 */
		cycled = 1;
		index = 0;
		end = writeback_index - 1;
		goto retry;
	}

	if (wbc->range_cyclic || (range_whole && wbc->nr_to_write > 0))
		mapping->writeback_index = done_index;

	return ret;
}


/**
 * gfs2_jdata_writepages - Write a bunch of dirty pages back to disk
 * @mapping: The mapping to write
 * @wbc: The writeback control
 * 
 */

static int gfs2_jdata_writepages(struct address_space *mapping,
				 struct writeback_control *wbc)
{
	struct gfs2_inode *ip = GFS2_I(mapping->host);
	struct gfs2_sbd *sdp = GFS2_SB(mapping->host);
	int ret;

	ret = gfs2_write_cache_jdata(mapping, wbc);
	if (ret == 0 && wbc->sync_mode == WB_SYNC_ALL) {
		gfs2_log_flush(sdp, ip->i_gl, NORMAL_FLUSH);
		ret = gfs2_write_cache_jdata(mapping, wbc);
	}
	return ret;
}

/**
 * stuffed_readpage - Fill in a Linux page with stuffed file data
 * @ip: the inode
 * @page: the page
 *
 * Returns: errno
 */

static int stuffed_readpage(struct gfs2_inode *ip, struct page *page)
{
	struct buffer_head *dibh;
	u64 dsize = i_size_read(&ip->i_inode);
	void *kaddr;
	int error;

	/*
	 * Due to the order of unstuffing files and ->fault(), we can be
	 * asked for a zero page in the case of a stuffed file being extended,
	 * so we need to supply one here. It doesn't happen often.
	 */
	if (unlikely(page->index)) {
		zero_user(page, 0, PAGE_SIZE);
		SetPageUptodate(page);
		return 0;
	}

	error = gfs2_meta_inode_buffer(ip, &dibh);
	if (error)
		return error;

	kaddr = kmap_atomic(page);
	if (dsize > (dibh->b_size - sizeof(struct gfs2_dinode)))
		dsize = (dibh->b_size - sizeof(struct gfs2_dinode));
	memcpy(kaddr, dibh->b_data + sizeof(struct gfs2_dinode), dsize);
	memset(kaddr + dsize, 0, PAGE_SIZE - dsize);
	kunmap_atomic(kaddr);
	flush_dcache_page(page);
	brelse(dibh);
	SetPageUptodate(page);

	return 0;
}


/**
 * __gfs2_readpage - readpage
 * @file: The file to read a page for
 * @page: The page to read
 *
 * This is the core of gfs2's readpage. Its used by the internal file
 * reading code as in that case we already hold the glock. Also its
 * called by gfs2_readpage() once the required lock has been granted.
 *
 */

static int __gfs2_readpage(struct file *file, struct page *page)
{
	struct gfs2_inode *ip = GFS2_I(page->mapping->host);
	struct gfs2_sbd *sdp = GFS2_SB(page->mapping->host);
	int error;

	if (gfs2_is_stuffed(ip)) {
		error = stuffed_readpage(ip, page);
		unlock_page(page);
	} else {
		error = mpage_readpage(page, gfs2_block_map);
	}

	if (unlikely(test_bit(SDF_SHUTDOWN, &sdp->sd_flags)))
		return -EIO;

	return error;
}

/**
 * gfs2_readpage - read a page of a file
 * @file: The file to read
 * @page: The page of the file
 *
 * This deals with the locking required. We have to unlock and
 * relock the page in order to get the locking in the right
 * order.
 */

static int gfs2_readpage(struct file *file, struct page *page)
{
	struct address_space *mapping = page->mapping;
	struct gfs2_inode *ip = GFS2_I(mapping->host);
	struct gfs2_holder gh;
	int error;

	unlock_page(page);
	gfs2_holder_init(ip->i_gl, LM_ST_SHARED, 0, &gh);
	error = gfs2_glock_nq(&gh);
	if (unlikely(error))
		goto out;
	error = AOP_TRUNCATED_PAGE;
	lock_page(page);
	if (page->mapping == mapping && !PageUptodate(page))
		error = __gfs2_readpage(file, page);
	else
		unlock_page(page);
	gfs2_glock_dq(&gh);
out:
	gfs2_holder_uninit(&gh);
	if (error && error != AOP_TRUNCATED_PAGE)
		lock_page(page);
	return error;
}

/**
 * gfs2_internal_read - read an internal file
 * @ip: The gfs2 inode
 * @buf: The buffer to fill
 * @pos: The file position
 * @size: The amount to read
 *
 */

int gfs2_internal_read(struct gfs2_inode *ip, char *buf, loff_t *pos,
                       unsigned size)
{
	struct address_space *mapping = ip->i_inode.i_mapping;
	unsigned long index = *pos / PAGE_SIZE;
	unsigned offset = *pos & (PAGE_SIZE - 1);
	unsigned copied = 0;
	unsigned amt;
	struct page *page;
	void *p;

	do {
		amt = size - copied;
		if (offset + size > PAGE_SIZE)
			amt = PAGE_SIZE - offset;
		page = read_cache_page(mapping, index, __gfs2_readpage, NULL);
		if (IS_ERR(page))
			return PTR_ERR(page);
		p = kmap_atomic(page);
		memcpy(buf + copied, p + offset, amt);
		kunmap_atomic(p);
		put_page(page);
		copied += amt;
		index++;
		offset = 0;
	} while(copied < size);
	(*pos) += size;
	return size;
}

/**
 * gfs2_readpages - Read a bunch of pages at once
 * @file: The file to read from
 * @mapping: Address space info
 * @pages: List of pages to read
 * @nr_pages: Number of pages to read
 *
 * Some notes:
 * 1. This is only for readahead, so we can simply ignore any things
 *    which are slightly inconvenient (such as locking conflicts between
 *    the page lock and the glock) and return having done no I/O. Its
 *    obviously not something we'd want to do on too regular a basis.
 *    Any I/O we ignore at this time will be done via readpage later.
 * 2. We don't handle stuffed files here we let readpage do the honours.
 * 3. mpage_readpages() does most of the heavy lifting in the common case.
 * 4. gfs2_block_map() is relied upon to set BH_Boundary in the right places.
 */

static int gfs2_readpages(struct file *file, struct address_space *mapping,
			  struct list_head *pages, unsigned nr_pages)
{
	struct inode *inode = mapping->host;
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_sbd *sdp = GFS2_SB(inode);
	struct gfs2_holder gh;
	int ret;

	gfs2_holder_init(ip->i_gl, LM_ST_SHARED, 0, &gh);
	ret = gfs2_glock_nq(&gh);
	if (unlikely(ret))
		goto out_uninit;
	if (!gfs2_is_stuffed(ip))
		ret = mpage_readpages(mapping, pages, nr_pages, gfs2_block_map);
	gfs2_glock_dq(&gh);
out_uninit:
	gfs2_holder_uninit(&gh);
	if (unlikely(test_bit(SDF_SHUTDOWN, &sdp->sd_flags)))
		ret = -EIO;
	return ret;
}

/**
 * gfs2_write_begin - Begin to write to a file
 * @file: The file to write to
 * @mapping: The mapping in which to write
 * @pos: The file offset at which to start writing
 * @len: Length of the write
 * @flags: Various flags
 * @pagep: Pointer to return the page
 * @fsdata: Pointer to return fs data (unused by GFS2)
 *
 * Returns: errno
 */

static int gfs2_write_begin(struct file *file, struct address_space *mapping,
			    loff_t pos, unsigned len, unsigned flags,
			    struct page **pagep, void **fsdata)
{
	struct gfs2_inode *ip = GFS2_I(mapping->host);
	struct gfs2_sbd *sdp = GFS2_SB(mapping->host);
	struct gfs2_inode *m_ip = GFS2_I(sdp->sd_statfs_inode);
	unsigned int data_blocks = 0, ind_blocks = 0, rblocks;
	unsigned requested = 0;
	int alloc_required;
	int error = 0;
	pgoff_t index = pos >> PAGE_SHIFT;
	unsigned from = pos & (PAGE_SIZE - 1);
	struct page *page;

	gfs2_holder_init(ip->i_gl, LM_ST_EXCLUSIVE, 0, &ip->i_gh);
	error = gfs2_glock_nq(&ip->i_gh);
	if (unlikely(error))
		goto out_uninit;
	if (&ip->i_inode == sdp->sd_rindex) {
		error = gfs2_glock_nq_init(m_ip->i_gl, LM_ST_EXCLUSIVE,
					   GL_NOCACHE, &m_ip->i_gh);
		if (unlikely(error)) {
			gfs2_glock_dq(&ip->i_gh);
			goto out_uninit;
		}
	}

	alloc_required = gfs2_write_alloc_required(ip, pos, len);

	if (alloc_required || gfs2_is_jdata(ip))
		gfs2_write_calc_reserv(ip, len, &data_blocks, &ind_blocks);

	if (alloc_required) {
		struct gfs2_alloc_parms ap = { .aflags = 0, };
		requested = data_blocks + ind_blocks;
		ap.target = requested;
		error = gfs2_quota_lock_check(ip, &ap);
		if (error)
			goto out_unlock;

		error = gfs2_inplace_reserve(ip, &ap);
		if (error)
			goto out_qunlock;
	}

	rblocks = RES_DINODE + ind_blocks;
	if (gfs2_is_jdata(ip))
		rblocks += data_blocks ? data_blocks : 1;
	if (ind_blocks || data_blocks)
		rblocks += RES_STATFS + RES_QUOTA;
	if (&ip->i_inode == sdp->sd_rindex)
		rblocks += 2 * RES_STATFS;
	if (alloc_required)
		rblocks += gfs2_rg_blocks(ip, requested);

	error = gfs2_trans_begin(sdp, rblocks,
				 PAGE_SIZE/sdp->sd_sb.sb_bsize);
	if (error)
		goto out_trans_fail;

	error = -ENOMEM;
	flags |= AOP_FLAG_NOFS;
	page = grab_cache_page_write_begin(mapping, index, flags);
	*pagep = page;
	if (unlikely(!page))
		goto out_endtrans;

	if (gfs2_is_stuffed(ip)) {
		error = 0;
		if (pos + len > sdp->sd_sb.sb_bsize - sizeof(struct gfs2_dinode)) {
			error = gfs2_unstuff_dinode(ip, page);
			if (error == 0)
				goto prepare_write;
		} else if (!PageUptodate(page)) {
			error = stuffed_readpage(ip, page);
		}
		goto out;
	}

prepare_write:
	error = __block_write_begin(page, from, len, gfs2_block_map);
out:
	if (error == 0)
		return 0;

	unlock_page(page);
	put_page(page);

	gfs2_trans_end(sdp);
	if (pos + len > ip->i_inode.i_size)
		gfs2_trim_blocks(&ip->i_inode);
	goto out_trans_fail;

out_endtrans:
	gfs2_trans_end(sdp);
out_trans_fail:
	if (alloc_required) {
		gfs2_inplace_release(ip);
out_qunlock:
		gfs2_quota_unlock(ip);
	}
out_unlock:
	if (&ip->i_inode == sdp->sd_rindex) {
		gfs2_glock_dq(&m_ip->i_gh);
		gfs2_holder_uninit(&m_ip->i_gh);
	}
	gfs2_glock_dq(&ip->i_gh);
out_uninit:
	gfs2_holder_uninit(&ip->i_gh);
	return error;
}

/**
 * adjust_fs_space - Adjusts the free space available due to gfs2_grow
 * @inode: the rindex inode
 */
static void adjust_fs_space(struct inode *inode)
{
	struct gfs2_sbd *sdp = inode->i_sb->s_fs_info;
	struct gfs2_inode *m_ip = GFS2_I(sdp->sd_statfs_inode);
	struct gfs2_inode *l_ip = GFS2_I(sdp->sd_sc_inode);
	struct gfs2_statfs_change_host *m_sc = &sdp->sd_statfs_master;
	struct gfs2_statfs_change_host *l_sc = &sdp->sd_statfs_local;
	struct buffer_head *m_bh, *l_bh;
	u64 fs_total, new_free;

	/* Total up the file system space, according to the latest rindex. */
	fs_total = gfs2_ri_total(sdp);
	if (gfs2_meta_inode_buffer(m_ip, &m_bh) != 0)
		return;

	spin_lock(&sdp->sd_statfs_spin);
	gfs2_statfs_change_in(m_sc, m_bh->b_data +
			      sizeof(struct gfs2_dinode));
	if (fs_total > (m_sc->sc_total + l_sc->sc_total))
		new_free = fs_total - (m_sc->sc_total + l_sc->sc_total);
	else
		new_free = 0;
	spin_unlock(&sdp->sd_statfs_spin);
	fs_warn(sdp, "File system extended by %llu blocks.\n",
		(unsigned long long)new_free);
	gfs2_statfs_change(sdp, new_free, new_free, 0);

	if (gfs2_meta_inode_buffer(l_ip, &l_bh) != 0)
		goto out;
	update_statfs(sdp, m_bh, l_bh);
	brelse(l_bh);
out:
	brelse(m_bh);
}

/**
 * gfs2_stuffed_write_end - Write end for stuffed files
 * @inode: The inode
 * @dibh: The buffer_head containing the on-disk inode
 * @pos: The file position
 * @len: The length of the write
 * @copied: How much was actually copied by the VFS
 * @page: The page
 *
 * This copies the data from the page into the inode block after
 * the inode data structure itself.
 *
 * Returns: errno
 */
static int gfs2_stuffed_write_end(struct inode *inode, struct buffer_head *dibh,
				  loff_t pos, unsigned len, unsigned copied,
				  struct page *page)
{
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_sbd *sdp = GFS2_SB(inode);
	struct gfs2_inode *m_ip = GFS2_I(sdp->sd_statfs_inode);
	u64 to = pos + copied;
	void *kaddr;
	unsigned char *buf = dibh->b_data + sizeof(struct gfs2_dinode);

	BUG_ON((pos + len) > (dibh->b_size - sizeof(struct gfs2_dinode)));
	kaddr = kmap_atomic(page);
	memcpy(buf + pos, kaddr + pos, copied);
	flush_dcache_page(page);
	kunmap_atomic(kaddr);

	WARN_ON(!PageUptodate(page));
	unlock_page(page);
	put_page(page);

	if (copied) {
		if (inode->i_size < to)
			i_size_write(inode, to);
		mark_inode_dirty(inode);
	}

	if (inode == sdp->sd_rindex) {
		adjust_fs_space(inode);
		sdp->sd_rindex_uptodate = 0;
	}

	brelse(dibh);
	gfs2_trans_end(sdp);
	if (inode == sdp->sd_rindex) {
		gfs2_glock_dq(&m_ip->i_gh);
		gfs2_holder_uninit(&m_ip->i_gh);
	}
	gfs2_glock_dq(&ip->i_gh);
	gfs2_holder_uninit(&ip->i_gh);
	return copied;
}

/**
 * gfs2_write_end
 * @file: The file to write to
 * @mapping: The address space to write to
 * @pos: The file position
 * @len: The length of the data
 * @copied: How much was actually copied by the VFS
 * @page: The page that has been written
 * @fsdata: The fsdata (unused in GFS2)
 *
 * The main write_end function for GFS2. We have a separate one for
 * stuffed files as they are slightly different, otherwise we just
 * put our locking around the VFS provided functions.
 *
 * Returns: errno
 */

static int gfs2_write_end(struct file *file, struct address_space *mapping,
			  loff_t pos, unsigned len, unsigned copied,
			  struct page *page, void *fsdata)
{
	struct inode *inode = page->mapping->host;
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_sbd *sdp = GFS2_SB(inode);
	struct gfs2_inode *m_ip = GFS2_I(sdp->sd_statfs_inode);
	struct buffer_head *dibh;
	unsigned int from = pos & (PAGE_SIZE - 1);
	unsigned int to = from + len;
	int ret;
	struct gfs2_trans *tr = current->journal_info;
	BUG_ON(!tr);

	BUG_ON(gfs2_glock_is_locked_by_me(ip->i_gl) == NULL);

	ret = gfs2_meta_inode_buffer(ip, &dibh);
	if (unlikely(ret)) {
		unlock_page(page);
		put_page(page);
		goto failed;
	}

	if (gfs2_is_stuffed(ip))
		return gfs2_stuffed_write_end(inode, dibh, pos, len, copied, page);

	if (!gfs2_is_writeback(ip))
		gfs2_page_add_databufs(ip, page, from, to);

	ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
	if (tr->tr_num_buf_new)
		__mark_inode_dirty(inode, I_DIRTY_DATASYNC);
	else
		gfs2_trans_add_meta(ip->i_gl, dibh);


	if (inode == sdp->sd_rindex) {
		adjust_fs_space(inode);
		sdp->sd_rindex_uptodate = 0;
	}

	brelse(dibh);
failed:
	gfs2_trans_end(sdp);
	gfs2_inplace_release(ip);
	if (ip->i_qadata && ip->i_qadata->qa_qd_num)
		gfs2_quota_unlock(ip);
	if (inode == sdp->sd_rindex) {
		gfs2_glock_dq(&m_ip->i_gh);
		gfs2_holder_uninit(&m_ip->i_gh);
	}
	gfs2_glock_dq(&ip->i_gh);
	gfs2_holder_uninit(&ip->i_gh);
	return ret;
}

/**
 * gfs2_set_page_dirty - Page dirtying function
 * @page: The page to dirty
 *
 * Returns: 1 if it dirtyed the page, or 0 otherwise
 */
 
static int gfs2_set_page_dirty(struct page *page)
{
	SetPageChecked(page);
	return __set_page_dirty_buffers(page);
}

/**
 * gfs2_bmap - Block map function
 * @mapping: Address space info
 * @lblock: The block to map
 *
 * Returns: The disk address for the block or 0 on hole or error
 */

static sector_t gfs2_bmap(struct address_space *mapping, sector_t lblock)
{
	struct gfs2_inode *ip = GFS2_I(mapping->host);
	struct gfs2_holder i_gh;
	sector_t dblock = 0;
	int error;

	error = gfs2_glock_nq_init(ip->i_gl, LM_ST_SHARED, LM_FLAG_ANY, &i_gh);
	if (error)
		return 0;

	if (!gfs2_is_stuffed(ip))
		dblock = generic_block_bmap(mapping, lblock, gfs2_block_map);

	gfs2_glock_dq_uninit(&i_gh);

	return dblock;
}

static void gfs2_discard(struct gfs2_sbd *sdp, struct buffer_head *bh)
{
	struct gfs2_bufdata *bd;

	lock_buffer(bh);
	gfs2_log_lock(sdp);
	clear_buffer_dirty(bh);
	bd = bh->b_private;
	if (bd) {
		if (!list_empty(&bd->bd_list) && !buffer_pinned(bh))
			list_del_init(&bd->bd_list);
		else
			gfs2_remove_from_journal(bh, REMOVE_JDATA);
	}
	bh->b_bdev = NULL;
	clear_buffer_mapped(bh);
	clear_buffer_req(bh);
	clear_buffer_new(bh);
	gfs2_log_unlock(sdp);
	unlock_buffer(bh);
}

static void gfs2_invalidatepage(struct page *page, unsigned int offset,
				unsigned int length)
{
	struct gfs2_sbd *sdp = GFS2_SB(page->mapping->host);
	unsigned int stop = offset + length;
	int partial_page = (offset || length < PAGE_SIZE);
	struct buffer_head *bh, *head;
	unsigned long pos = 0;

	BUG_ON(!PageLocked(page));
	if (!partial_page)
		ClearPageChecked(page);
	if (!page_has_buffers(page))
		goto out;

	bh = head = page_buffers(page);
	do {
		if (pos + bh->b_size > stop)
			return;

		if (offset <= pos)
			gfs2_discard(sdp, bh);
		pos += bh->b_size;
		bh = bh->b_this_page;
	} while (bh != head);
out:
	if (!partial_page)
		try_to_release_page(page, 0);
}

/**
 * gfs2_ok_for_dio - check that dio is valid on this file
 * @ip: The inode
 * @offset: The offset at which we are reading or writing
 *
 * Returns: 0 (to ignore the i/o request and thus fall back to buffered i/o)
 *          1 (to accept the i/o request)
 */
static int gfs2_ok_for_dio(struct gfs2_inode *ip, loff_t offset)
{
	/*
	 * Should we return an error here? I can't see that O_DIRECT for
	 * a stuffed file makes any sense. For now we'll silently fall
	 * back to buffered I/O
	 */
	if (gfs2_is_stuffed(ip))
		return 0;

	if (offset >= i_size_read(&ip->i_inode))
		return 0;
	return 1;
}



static ssize_t gfs2_direct_IO(struct kiocb *iocb, struct iov_iter *iter)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;
	struct address_space *mapping = inode->i_mapping;
	struct gfs2_inode *ip = GFS2_I(inode);
	loff_t offset = iocb->ki_pos;
	struct gfs2_holder gh;
	int rv;

	/*
	 * Deferred lock, even if its a write, since we do no allocation
	 * on this path. All we need change is atime, and this lock mode
	 * ensures that other nodes have flushed their buffered read caches
	 * (i.e. their page cache entries for this inode). We do not,
	 * unfortunately have the option of only flushing a range like
	 * the VFS does.
	 */
	gfs2_holder_init(ip->i_gl, LM_ST_DEFERRED, 0, &gh);
	rv = gfs2_glock_nq(&gh);
	if (rv)
		goto out_uninit;
	rv = gfs2_ok_for_dio(ip, offset);
	if (rv != 1)
		goto out; /* dio not valid, fall back to buffered i/o */

	/*
	 * Now since we are holding a deferred (CW) lock at this point, you
	 * might be wondering why this is ever needed. There is a case however
	 * where we've granted a deferred local lock against a cached exclusive
	 * glock. That is ok provided all granted local locks are deferred, but
	 * it also means that it is possible to encounter pages which are
	 * cached and possibly also mapped. So here we check for that and sort
	 * them out ahead of the dio. The glock state machine will take care of
	 * everything else.
	 *
	 * If in fact the cached glock state (gl->gl_state) is deferred (CW) in
	 * the first place, mapping->nr_pages will always be zero.
	 */
	if (mapping->nrpages) {
		loff_t lstart = offset & ~(PAGE_SIZE - 1);
		loff_t len = iov_iter_count(iter);
		loff_t end = PAGE_ALIGN(offset + len) - 1;

		rv = 0;
		if (len == 0)
			goto out;
		if (test_and_clear_bit(GIF_SW_PAGED, &ip->i_flags))
			unmap_shared_mapping_range(ip->i_inode.i_mapping, offset, len);
		rv = filemap_write_and_wait_range(mapping, lstart, end);
		if (rv)
			goto out;
		if (iov_iter_rw(iter) == WRITE)
			truncate_inode_pages_range(mapping, lstart, end);
	}

	rv = __blockdev_direct_IO(iocb, inode, inode->i_sb->s_bdev, iter,
				  gfs2_get_block_direct, NULL, NULL, 0);
out:
	gfs2_glock_dq(&gh);
out_uninit:
	gfs2_holder_uninit(&gh);
	return rv;
}

/**
 * gfs2_releasepage - free the metadata associated with a page
 * @page: the page that's being released
 * @gfp_mask: passed from Linux VFS, ignored by us
 *
 * Call try_to_free_buffers() if the buffers in this page can be
 * released.
 *
 * Returns: 0
 */

int gfs2_releasepage(struct page *page, gfp_t gfp_mask)
{
	struct address_space *mapping = page->mapping;
	struct gfs2_sbd *sdp = gfs2_mapping2sbd(mapping);
	struct buffer_head *bh, *head;
	struct gfs2_bufdata *bd;

	if (!page_has_buffers(page))
		return 0;

	/*
	 * From xfs_vm_releasepage: mm accommodates an old ext3 case where
	 * clean pages might not have had the dirty bit cleared.  Thus, it can
	 * send actual dirty pages to ->releasepage() via shrink_active_list().
	 *
	 * As a workaround, we skip pages that contain dirty buffers below.
	 * Once ->releasepage isn't called on dirty pages anymore, we can warn
	 * on dirty buffers like we used to here again.
	 */

	gfs2_log_lock(sdp);
	spin_lock(&sdp->sd_ail_lock);
	head = bh = page_buffers(page);
	do {
		if (atomic_read(&bh->b_count))
			goto cannot_release;
		bd = bh->b_private;
		if (bd && bd->bd_tr)
			goto cannot_release;
		if (buffer_dirty(bh) || WARN_ON(buffer_pinned(bh)))
			goto cannot_release;
		bh = bh->b_this_page;
	} while(bh != head);
	spin_unlock(&sdp->sd_ail_lock);

	head = bh = page_buffers(page);
	do {
		bd = bh->b_private;
		if (bd) {
			gfs2_assert_warn(sdp, bd->bd_bh == bh);
			if (!list_empty(&bd->bd_list))
				list_del_init(&bd->bd_list);
			bd->bd_bh = NULL;
			bh->b_private = NULL;
			kmem_cache_free(gfs2_bufdata_cachep, bd);
		}

		bh = bh->b_this_page;
	} while (bh != head);
	gfs2_log_unlock(sdp);

	return try_to_free_buffers(page);

cannot_release:
	spin_unlock(&sdp->sd_ail_lock);
	gfs2_log_unlock(sdp);
	return 0;
}

static const struct address_space_operations gfs2_writeback_aops = {
	.writepage = gfs2_writepage,
	.writepages = gfs2_writepages,
	.readpage = gfs2_readpage,
	.readpages = gfs2_readpages,
	.write_begin = gfs2_write_begin,
	.write_end = gfs2_write_end,
	.bmap = gfs2_bmap,
	.invalidatepage = gfs2_invalidatepage,
	.releasepage = gfs2_releasepage,
	.direct_IO = gfs2_direct_IO,
	.migratepage = buffer_migrate_page,
	.is_partially_uptodate = block_is_partially_uptodate,
	.error_remove_page = generic_error_remove_page,
};

static const struct address_space_operations gfs2_ordered_aops = {
	.writepage = gfs2_writepage,
	.writepages = gfs2_writepages,
	.readpage = gfs2_readpage,
	.readpages = gfs2_readpages,
	.write_begin = gfs2_write_begin,
	.write_end = gfs2_write_end,
	.set_page_dirty = gfs2_set_page_dirty,
	.bmap = gfs2_bmap,
	.invalidatepage = gfs2_invalidatepage,
	.releasepage = gfs2_releasepage,
	.direct_IO = gfs2_direct_IO,
	.migratepage = buffer_migrate_page,
	.is_partially_uptodate = block_is_partially_uptodate,
	.error_remove_page = generic_error_remove_page,
};

static const struct address_space_operations gfs2_jdata_aops = {
	.writepage = gfs2_jdata_writepage,
	.writepages = gfs2_jdata_writepages,
	.readpage = gfs2_readpage,
	.readpages = gfs2_readpages,
	.write_begin = gfs2_write_begin,
	.write_end = gfs2_write_end,
	.set_page_dirty = gfs2_set_page_dirty,
	.bmap = gfs2_bmap,
	.invalidatepage = gfs2_invalidatepage,
	.releasepage = gfs2_releasepage,
	.is_partially_uptodate = block_is_partially_uptodate,
	.error_remove_page = generic_error_remove_page,
};

void gfs2_set_aops(struct inode *inode)
{
	struct gfs2_inode *ip = GFS2_I(inode);

	if (gfs2_is_writeback(ip))
		inode->i_mapping->a_ops = &gfs2_writeback_aops;
	else if (gfs2_is_ordered(ip))
		inode->i_mapping->a_ops = &gfs2_ordered_aops;
	else if (gfs2_is_jdata(ip))
		inode->i_mapping->a_ops = &gfs2_jdata_aops;
	else
		BUG();
}

