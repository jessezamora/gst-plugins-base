/*
 * Copyright (c) 2016, Freescale Semiconductor, Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <linux/ion.h>

#include <gst/allocators/gstdmabuf.h>
#include "gstionmemory.h"

GST_DEBUG_CATEGORY_STATIC (ion_allocator_debug);
#define GST_CAT_DEFAULT ion_allocator_debug

#define gst_ion_allocator_parent_class parent_class

G_DEFINE_TYPE (GstIONAllocator, gst_ion_allocator, GST_TYPE_DMABUF_ALLOCATOR)

static gint
gst_ion_ioctl (gint fd, gint req, void *arg)
{
  gint ret = ioctl (fd, req, arg);
  if (ret < 0) {
    GST_ERROR ("ioctl %x failed with code %d: %s\n", req, ret,
        strerror (errno));
  }
  return ret;
}

static void
gst_ion_mem_init (void)
{
  GstAllocator *allocator = g_object_new (gst_ion_allocator_get_type (), NULL);
  GstIONAllocator *self = GST_ION_ALLOCATOR (allocator);
  gint fd;

  fd = open ("/dev/ion", O_RDWR);
  if (fd < 0) {
    GST_ERROR ("Could not open ion driver");
    g_object_unref (self);
    return;
  }

  self->fd = fd;

  gst_allocator_register (GST_ALLOCATOR_ION, allocator);
}

GstAllocator *
gst_ion_allocator_obtain (void)
{
  static GOnce ion_allocator_once = G_ONCE_INIT;
  GstAllocator *allocator;

  g_once (&ion_allocator_once, (GThreadFunc) gst_ion_mem_init, NULL);

  allocator = gst_allocator_find (GST_ALLOCATOR_ION);
  if (allocator == NULL)
    GST_WARNING ("No allocator named %s found", GST_ALLOCATOR_ION);

  return allocator;
}

static GstMemory *
gst_ion_alloc_alloc (GstAllocator * allocator, gsize size,
    GstAllocationParams * params)
{
  GstIONAllocator *self = GST_ION_ALLOCATOR (allocator);
  struct ion_allocation_data allocation_data = { 0 };
  struct ion_fd_data fd_data = { 0 };
  struct ion_handle_data handle_data = { 0 };
  ion_user_handle_t ion_handle;
  GstMemory *mem;
  gsize ion_size;
  gint dma_fd = -1;
  gint ret;

  if (self->fd < 0) {
    GST_ERROR ("ion allocate param wrong");
    return NULL;
  }

  ion_size = size + params->prefix + params->padding;
  allocation_data.len = ion_size;
  allocation_data.align = params->align;
  allocation_data.heap_id_mask = ION_HEAP_TYPE_DMA_MASK;
  allocation_data.flags = 0;
  if (gst_ion_ioctl (self->fd, ION_IOC_ALLOC, &allocation_data) < 0) {
    GST_ERROR ("ion allocate failed.");
    return NULL;
  }
  ion_handle = allocation_data.handle;

  fd_data.handle = ion_handle;
  ret = gst_ion_ioctl (self->fd, ION_IOC_MAP, &fd_data);
  if (ret < 0 || fd_data.fd < 0) {
    GST_ERROR ("map ioctl failed or returned negative fd");
    goto bail;
  }
  dma_fd = fd_data.fd;

  mem = gst_dmabuf_allocator_alloc (allocator, dma_fd, size);

  GST_DEBUG ("ion allocated size: %" G_GSIZE_FORMAT "DMA FD: %d", ion_size,
      dma_fd);

  return mem;

bail:
  if (dma_fd >= 0) {
    close (dma_fd);
  }
  handle_data.handle = ion_handle;
  gst_ion_ioctl (self->fd, ION_IOC_FREE, &handle_data);

  return NULL;
}

static void
gst_ion_alloc_free (GstAllocator * allocator, GstMemory * memory)
{
  GstIONAllocator *self = GST_ION_ALLOCATOR (allocator);
  struct ion_handle_data handle_data = { 0 };
  struct ion_fd_data fd_data = { 0 };
  gint ret;
  gint fd;

  if (self->fd < 0 || !memory) {
    GST_ERROR ("ion free param wrong");
    return;
  }

  if (!gst_is_dmabuf_memory (memory)) {
    GST_ERROR ("isn't dmabuf memory");
    return;
  }

  fd = gst_dmabuf_memory_get_fd (memory);
  if (fd < 0) {
    GST_ERROR ("dmabuf memory get fd failed");
    return;
  }

  GST_DEBUG ("ion free DMA FD: %d", fd);

  fd_data.fd = fd;
  ret = gst_ion_ioctl (self->fd, ION_IOC_IMPORT, &fd_data);
  if (ret < 0) {
    GST_ERROR ("import ioctl failed");
    return;
  }

  close (fd);
  handle_data.handle = fd_data.handle;
  /* ION_IOC_IMPORT will add one ref for ion handle. */
  gst_ion_ioctl (self->fd, ION_IOC_FREE, &handle_data);
  gst_ion_ioctl (self->fd, ION_IOC_FREE, &handle_data);
}

static void
gst_ion_allocator_dispose (GObject * object)
{
  GstIONAllocator *self = GST_ION_ALLOCATOR (object);

  if (self->fd > 0) {
    close (self->fd);
    self->fd = -1;
  }

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_ion_allocator_class_init (GstIONAllocatorClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstAllocatorClass *allocator_class = GST_ALLOCATOR_CLASS (klass);
  GstDmaBufAllocatorClass *dmabuf_allocator_class = GST_DMABUF_ALLOCATOR_CLASS (klass);

  allocator_class->alloc = GST_DEBUG_FUNCPTR (gst_ion_alloc_alloc);
  dmabuf_allocator_class->free = GST_DEBUG_FUNCPTR (gst_ion_alloc_free);
  gobject_class->dispose = GST_DEBUG_FUNCPTR (gst_ion_allocator_dispose);

  GST_DEBUG_CATEGORY_INIT (ion_allocator_debug, "ionmemory", 0,
      "DMA FD memory allocator based on ion");
}

static void
gst_ion_allocator_init (GstIONAllocator * self)
{
  GstAllocator *allocator = GST_ALLOCATOR (self);

  allocator->mem_type = GST_ALLOCATOR_ION;
}
