/*
 * Copyright 2020 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "util/format/u_format.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "util/u_box.h"
#include "util/log.h"
#include "pipe/p_context.h"
#include "pipe-loader/pipe_loader.h"
#include "state_tracker/st_context.h"
#include "os/os_process.h"
#include "zink/zink_public.h"
#include "zink/zink_kopper.h"
#include "driver_trace/tr_screen.h"

#include "dri_screen.h"
#include "dri_context.h"
#include "dri_drawable.h"
#include "dri_helpers.h"
#include "dri_query_renderer.h"

#include <vulkan/vulkan.h>

#ifdef VK_USE_PLATFORM_XCB_KHR
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/xfixes.h>
#include "util/libsync.h"
#include <X11/Xlib-xcb.h>
#include "drm-uapi/drm_fourcc.h"
#endif

struct kopper_drawable {
   struct dri_drawable base;
   struct kopper_loader_info info;
   __DRIimage   *image; //texture_from_pixmap
   bool is_window;
};

struct kopper_screen {
   struct dri_screen base;
   struct pipe_screen *screen; //unwrapped
   bool has_dmabuf;
   bool has_modifiers;
   bool is_sw;
};

extern const __DRIimageExtension driVkImageExtension;
extern const __DRIimageExtension driVkImageExtensionSw;

static void
kopper_flush_drawable(__DRIdrawable *dPriv)
{
   dri_flush(dPriv->driContextPriv, dPriv, __DRI2_FLUSH_DRAWABLE, -1);
}

static inline void
kopper_invalidate_drawable(__DRIdrawable *dPriv)
{
   struct dri_drawable *drawable = dri_drawable(dPriv);

   drawable->texture_stamp = dPriv->lastStamp - 1;

   p_atomic_inc(&drawable->base.stamp);
}

static const __DRI2flushExtension driVkFlushExtension = {
    .base = { __DRI2_FLUSH, 4 },

    .flush                = kopper_flush_drawable,
    .invalidate           = kopper_invalidate_drawable,
    .flush_with_flags     = dri_flush,
};

static const __DRIrobustnessExtension dri2Robustness = {
   .base = { __DRI2_ROBUSTNESS, 1 }
};

const __DRIkopperExtension driKopperExtension;

static const __DRIextension *drivk_screen_extensions[] = {
   &driTexBufferExtension.base,
   &dri2RendererQueryExtension.base,
   &dri2ConfigQueryExtension.base,
   &dri2FenceExtension.base,
   &dri2Robustness.base,
   &driVkImageExtension.base,
   &dri2FlushControlExtension.base,
   &driVkFlushExtension.base,
   &driKopperExtension.base,
   NULL
};

static const __DRIextension *drivk_sw_screen_extensions[] = {
   &driTexBufferExtension.base,
   &dri2RendererQueryExtension.base,
   &dri2ConfigQueryExtension.base,
   &dri2FenceExtension.base,
   &dri2Robustness.base,
   &driVkImageExtensionSw.base,
   &dri2FlushControlExtension.base,
   &driVkFlushExtension.base,
   NULL
};

static const __DRIconfig **
kopper_init_screen(__DRIscreen * sPriv)
{
   const __DRIconfig **configs;
   struct dri_screen *screen;
   struct kopper_screen *kscreen;
   struct pipe_screen *pscreen = NULL;

   if (!sPriv->kopper_loader) {
      fprintf(stderr, "mesa: Kopper interface not found!\n"
                      "      Ensure the versions of %s built with this version of Zink are\n"
                      "      in your library path!\n", KOPPER_LIB_NAMES);
      return NULL;
   }
   kscreen = CALLOC_STRUCT(kopper_screen);
   if (!kscreen)
      return NULL;
   screen = &kscreen->base;

   screen->sPriv = sPriv;
   screen->fd = sPriv->fd;
   screen->can_share_buffer = true;

   sPriv->driverPrivate = (void *)kscreen;

   bool success;
   if (screen->fd != -1)
      success = pipe_loader_drm_probe_fd(&screen->dev, screen->fd);
   else
      success = pipe_loader_vk_probe_dri(&screen->dev, NULL);
   if (success) {
      pscreen = pipe_loader_create_screen(screen->dev);
      dri_init_options(screen);
   }

   if (!pscreen)
      goto fail;

   kscreen->screen = trace_screen_unwrap(pscreen);

   configs = dri_init_screen_helper(screen, pscreen);
   if (!configs)
      goto fail;

   assert(pscreen->get_param(pscreen, PIPE_CAP_DEVICE_RESET_STATUS_QUERY));
   screen->has_reset_status_query = true;
   screen->lookup_egl_image = dri2_lookup_egl_image;
   kscreen->has_dmabuf = pscreen->get_param(pscreen, PIPE_CAP_DMABUF);
   kscreen->has_modifiers = pscreen->query_dmabuf_modifiers != NULL;
   kscreen->is_sw = zink_kopper_is_cpu(pscreen);
   if (kscreen->has_dmabuf)
      sPriv->extensions = drivk_screen_extensions;
   else
      sPriv->extensions = drivk_sw_screen_extensions;

   const __DRIimageLookupExtension *image = sPriv->dri2.image;
   if (image &&
       image->base.version >= 2 &&
       image->validateEGLImage &&
       image->lookupEGLImageValidated) {
      screen->validate_egl_image = dri2_validate_egl_image;
      screen->lookup_egl_image_validated = dri2_lookup_egl_image_validated;
   }

   return configs;
fail:
   dri_destroy_screen_helper(screen);
   if (screen->dev)
      pipe_loader_release(&screen->dev, 1);
   return NULL;
}

// copypasta alert

static inline void
drisw_present_texture(struct pipe_context *pipe, __DRIdrawable *dPriv,
                      struct pipe_resource *ptex, struct pipe_box *sub_box)
{
   struct dri_drawable *drawable = dri_drawable(dPriv);
   struct dri_screen *screen = dri_screen(drawable->sPriv);

   screen->base.screen->flush_frontbuffer(screen->base.screen, pipe, ptex, 0, 0, drawable, sub_box);
}

extern bool
dri_image_drawable_get_buffers(struct dri_drawable *drawable,
                               struct __DRIimageList *images,
                               const enum st_attachment_type *statts,
                               unsigned statts_count);

#ifdef VK_USE_PLATFORM_XCB_KHR
static int
get_dri_format(enum pipe_format pf)
{
   int image_format;
   switch (pf) {
   case PIPE_FORMAT_R16G16B16A16_FLOAT:
      image_format = __DRI_IMAGE_FORMAT_ABGR16161616F;
      break;
   case PIPE_FORMAT_R16G16B16X16_FLOAT:
      image_format = __DRI_IMAGE_FORMAT_XBGR16161616F;
      break;
   case PIPE_FORMAT_B5G5R5A1_UNORM:
      image_format = __DRI_IMAGE_FORMAT_ARGB1555;
      break;
   case PIPE_FORMAT_B5G6R5_UNORM:
      image_format = __DRI_IMAGE_FORMAT_RGB565;
      break;
   case PIPE_FORMAT_BGRX8888_UNORM:
      image_format = __DRI_IMAGE_FORMAT_XRGB8888;
      break;
   case PIPE_FORMAT_BGRA8888_UNORM:
      image_format = __DRI_IMAGE_FORMAT_ARGB8888;
      break;
   case PIPE_FORMAT_RGBX8888_UNORM:
      image_format = __DRI_IMAGE_FORMAT_XBGR8888;
      break;
   case PIPE_FORMAT_RGBA8888_UNORM:
      image_format = __DRI_IMAGE_FORMAT_ABGR8888;
      break;
   case PIPE_FORMAT_B10G10R10X2_UNORM:
      image_format = __DRI_IMAGE_FORMAT_XRGB2101010;
      break;
   case PIPE_FORMAT_B10G10R10A2_UNORM:
      image_format = __DRI_IMAGE_FORMAT_ARGB2101010;
      break;
   case PIPE_FORMAT_R10G10B10X2_UNORM:
      image_format = __DRI_IMAGE_FORMAT_XBGR2101010;
      break;
   case PIPE_FORMAT_R10G10B10A2_UNORM:
      image_format = __DRI_IMAGE_FORMAT_ABGR2101010;
      break;
   default:
      image_format = __DRI_IMAGE_FORMAT_NONE;
      break;
   }
   return image_format;
}

/* the DRIimage createImage function takes __DRI_IMAGE_FORMAT codes, while
 * the createImageFromFds call takes DRM_FORMAT codes. To avoid
 * complete confusion, just deal in __DRI_IMAGE_FORMAT codes for now and
 * translate to DRM_FORMAT codes in the call to createImageFromFds
 */
static int
image_format_to_fourcc(int format)
{

   /* Convert from __DRI_IMAGE_FORMAT to DRM_FORMAT (sigh) */
   switch (format) {
   case __DRI_IMAGE_FORMAT_SARGB8: return __DRI_IMAGE_FOURCC_SARGB8888;
   case __DRI_IMAGE_FORMAT_SABGR8: return __DRI_IMAGE_FOURCC_SABGR8888;
   case __DRI_IMAGE_FORMAT_SXRGB8: return __DRI_IMAGE_FOURCC_SXRGB8888;
   case __DRI_IMAGE_FORMAT_RGB565: return DRM_FORMAT_RGB565;
   case __DRI_IMAGE_FORMAT_XRGB8888: return DRM_FORMAT_XRGB8888;
   case __DRI_IMAGE_FORMAT_ARGB8888: return DRM_FORMAT_ARGB8888;
   case __DRI_IMAGE_FORMAT_ABGR8888: return DRM_FORMAT_ABGR8888;
   case __DRI_IMAGE_FORMAT_XBGR8888: return DRM_FORMAT_XBGR8888;
   case __DRI_IMAGE_FORMAT_XRGB2101010: return DRM_FORMAT_XRGB2101010;
   case __DRI_IMAGE_FORMAT_ARGB2101010: return DRM_FORMAT_ARGB2101010;
   case __DRI_IMAGE_FORMAT_XBGR2101010: return DRM_FORMAT_XBGR2101010;
   case __DRI_IMAGE_FORMAT_ABGR2101010: return DRM_FORMAT_ABGR2101010;
   case __DRI_IMAGE_FORMAT_XBGR16161616F: return DRM_FORMAT_XBGR16161616F;
   case __DRI_IMAGE_FORMAT_ABGR16161616F: return DRM_FORMAT_ABGR16161616F;
   }
   return 0;
}

#ifdef HAVE_DRI3_MODIFIERS
static __DRIimage *
dri3_create_image_from_buffers(xcb_connection_t *c,
                               xcb_dri3_buffers_from_pixmap_reply_t *bp_reply,
                               unsigned int format,
                               __DRIscreen *dri_screen,
                               const __DRIimageExtension *image,
                               void *loaderPrivate)
{
   __DRIimage                           *ret;
   int                                  *fds;
   uint32_t                             *strides_in, *offsets_in;
   int                                   strides[4], offsets[4];
   unsigned                              error;
   int                                   i;

   if (bp_reply->nfd > 4)
      return NULL;

   fds = xcb_dri3_buffers_from_pixmap_reply_fds(c, bp_reply);
   strides_in = xcb_dri3_buffers_from_pixmap_strides(bp_reply);
   offsets_in = xcb_dri3_buffers_from_pixmap_offsets(bp_reply);
   for (i = 0; i < bp_reply->nfd; i++) {
      strides[i] = strides_in[i];
      offsets[i] = offsets_in[i];
   }

   ret = image->createImageFromDmaBufs2(dri_screen,
                                        bp_reply->width,
                                        bp_reply->height,
                                        image_format_to_fourcc(format),
                                        bp_reply->modifier,
                                        fds, bp_reply->nfd,
                                        strides, offsets,
                                        0, 0, 0, 0, /* UNDEFINED */
                                        &error, loaderPrivate);

   for (i = 0; i < bp_reply->nfd; i++)
      close(fds[i]);

   return ret;
}
#endif

static __DRIimage *
dri3_create_image(xcb_connection_t *c,
                  xcb_dri3_buffer_from_pixmap_reply_t *bp_reply,
                  unsigned int format,
                  __DRIscreen *dri_screen,
                  const __DRIimageExtension *image,
                  void *loaderPrivate)
{
   int                                  *fds;
   __DRIimage                           *image_planar, *ret;
   int                                  stride, offset;

   /* Get an FD for the pixmap object
    */
   fds = xcb_dri3_buffer_from_pixmap_reply_fds(c, bp_reply);

   stride = bp_reply->stride;
   offset = 0;

   /* createImageFromFds creates a wrapper __DRIimage structure which
    * can deal with multiple planes for things like Yuv images. So, once
    * we've gotten the planar wrapper, pull the single plane out of it and
    * discard the wrapper.
    */
   image_planar = image->createImageFromFds(dri_screen,
                                            bp_reply->width,
                                            bp_reply->height,
                                            image_format_to_fourcc(format),
                                            fds, 1,
                                            &stride, &offset, loaderPrivate);
   close(fds[0]);
   if (!image_planar)
      return NULL;

   ret = image->fromPlanar(image_planar, 0, loaderPrivate);

   if (!ret)
      ret = image_planar;
   else
      image->destroyImage(image_planar);

   return ret;
}


static void
handle_in_fence(__DRIcontext *context, __DRIimage *img)
{
   struct dri_context *ctx = dri_context(context);
   struct pipe_context *pipe = ctx->st->pipe;
   struct pipe_fence_handle *fence;
   int fd = img->in_fence_fd;

   if (fd == -1)
      return;

   validate_fence_fd(fd);

   img->in_fence_fd = -1;

   pipe->create_fence_fd(pipe, &fence, fd, PIPE_FD_TYPE_NATIVE_SYNC);
   pipe->fence_server_sync(pipe, fence);
   pipe->screen->fence_reference(pipe->screen, &fence, NULL);

   close(fd);
}

/** kopper_get_pixmap_buffer
 *
 * Get the DRM object for a pixmap from the X server and
 * wrap that with a __DRIimage structure using createImageFromFds
 */
static struct pipe_resource *
kopper_get_pixmap_buffer(struct kopper_drawable *cdraw,
                         enum pipe_format pf)
{
   xcb_drawable_t                       pixmap;
   int                                  width;
   int                                  height;
   int format = get_dri_format(pf);
   __DRIscreen                          *cur_screen;
   struct kopper_loader_info *info = &cdraw->info;
   assert(info->bos.sType == VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR);
   xcb_connection_t *conn = info->xcb.connection;
   pixmap = info->xcb.window;

   if (cdraw->image)
      return cdraw->image->texture;

   /* FIXME: probably broken for OBS studio?
    * see dri3_get_pixmap_buffer()
    */
   cur_screen = cdraw->base.sPriv;

#ifdef HAVE_DRI3_MODIFIERS
   struct kopper_screen *kscreen = (struct kopper_screen*)cur_screen->driverPrivate;
   if (kscreen->has_modifiers) {
      xcb_dri3_buffers_from_pixmap_cookie_t bps_cookie;
      xcb_dri3_buffers_from_pixmap_reply_t *bps_reply;
      xcb_generic_error_t *error;

      bps_cookie = xcb_dri3_buffers_from_pixmap(conn, pixmap);
      bps_reply = xcb_dri3_buffers_from_pixmap_reply(conn, bps_cookie, &error);
      if (!bps_reply) {
         mesa_loge("kopper: could not create texture from pixmap (%u)", error->error_code);
         return NULL;
      }
      cdraw->image =
         dri3_create_image_from_buffers(conn, bps_reply, format,
                                        cur_screen, &driVkImageExtension,
                                        cdraw);
      width = bps_reply->width;
      height = bps_reply->height;
      free(bps_reply);
   } else
#endif
   {
      xcb_dri3_buffer_from_pixmap_cookie_t bp_cookie;
      xcb_dri3_buffer_from_pixmap_reply_t *bp_reply;
      xcb_generic_error_t *error;

      bp_cookie = xcb_dri3_buffer_from_pixmap(conn, pixmap);
      bp_reply = xcb_dri3_buffer_from_pixmap_reply(conn, bp_cookie, &error);
      if (!bp_reply) {
         mesa_loge("kopper: could not create texture from pixmap (%u)", error->error_code);
         return NULL;
      }

      cdraw->image = dri3_create_image(conn, bp_reply, format,
                                       cur_screen, &driVkImageExtension,
                                       cdraw);
      width = bp_reply->width;
      height = bp_reply->height;
      free(bp_reply);
   }

   cdraw->base.dPriv->w = width;
   cdraw->base.dPriv->h = height;

   return cdraw->image->texture;
}
#endif //VK_USE_PLATFORM_XCB_KHR

static void
kopper_allocate_textures(struct dri_context *ctx,
                         struct dri_drawable *drawable,
                         const enum st_attachment_type *statts,
                         unsigned statts_count)
{
   struct dri_screen *screen = dri_screen(drawable->sPriv);
   struct pipe_resource templ;
   unsigned width, height;
   boolean resized;
   unsigned i;
   struct __DRIimageList images;
   __DRIdrawable *dri_drawable = drawable->dPriv;
   const __DRIimageLoaderExtension *image = drawable->sPriv->image.loader;
   struct kopper_drawable *cdraw = (struct kopper_drawable *)drawable;
   struct kopper_screen *kscreen = (struct kopper_screen*)drawable->sPriv->driverPrivate;

   bool is_window = cdraw->is_window;
   bool is_pixmap = !is_window && cdraw->info.bos.sType == VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;

   width  = drawable->dPriv->w;
   height = drawable->dPriv->h;

   resized = (drawable->old_w != width ||
              drawable->old_h != height);

   /* Wait for glthread to finish because we can't use pipe_context from
    * multiple threads.
    */
   if (ctx->st->thread_finish)
      ctx->st->thread_finish(ctx->st);

   /* First get the buffers from the loader */
   if (image) {
      if (!dri_image_drawable_get_buffers(drawable, &images,
                                          statts, statts_count))
         return;
   }

   if (image) {
      if (images.image_mask & __DRI_IMAGE_BUFFER_FRONT) {
         struct pipe_resource **buf =
            &drawable->textures[ST_ATTACHMENT_FRONT_LEFT];
         struct pipe_resource *texture = images.front->texture;

         dri_drawable->w = texture->width0;
         dri_drawable->h = texture->height0;

         pipe_resource_reference(buf, texture);
      }

      if (images.image_mask & __DRI_IMAGE_BUFFER_BACK) {
         struct pipe_resource **buf =
            &drawable->textures[ST_ATTACHMENT_BACK_LEFT];
         struct pipe_resource *texture = images.back->texture;

         dri_drawable->w = texture->width0;
         dri_drawable->h = texture->height0;

         pipe_resource_reference(buf, texture);
      }

      if (images.image_mask & __DRI_IMAGE_BUFFER_SHARED) {
         struct pipe_resource **buf =
            &drawable->textures[ST_ATTACHMENT_BACK_LEFT];
         struct pipe_resource *texture = images.back->texture;

         dri_drawable->w = texture->width0;
         dri_drawable->h = texture->height0;

         pipe_resource_reference(buf, texture);

         ctx->is_shared_buffer_bound = true;
      } else {
         ctx->is_shared_buffer_bound = false;
      }
   } else {
      /* remove outdated textures */
      if (resized) {
         for (i = 0; i < ST_ATTACHMENT_COUNT; i++) {
            if (drawable->textures[i] && i < ST_ATTACHMENT_DEPTH_STENCIL && !is_pixmap) {
               drawable->textures[i]->width0 = width;
               drawable->textures[i]->height0 = height;
               /* force all contexts to revalidate framebuffer */
               p_atomic_inc(&drawable->base.stamp);
            } else
               pipe_resource_reference(&drawable->textures[i], NULL);
            pipe_resource_reference(&drawable->msaa_textures[i], NULL);
            if (is_pixmap && i == ST_ATTACHMENT_FRONT_LEFT) {
               FREE(cdraw->image);
               cdraw->image = NULL;
            }
         }
      }
   }

   drawable->old_w = width;
   drawable->old_h = height;

   memset(&templ, 0, sizeof(templ));
   templ.target = screen->target;
   templ.width0 = width;
   templ.height0 = height;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.last_level = 0;

#if 0
XXX do this once swapinterval is hooked up
   /* pixmaps always have front buffers.
    * Exchange swaps also mandate fake front buffers.
    */
   if (draw->type != LOADER_DRI3_DRAWABLE_WINDOW ||
       draw->swap_method == __DRI_ATTRIB_SWAP_EXCHANGE)
      buffer_mask |= __DRI_IMAGE_BUFFER_FRONT;
#endif

   uint32_t attachments = 0;
   for (i = 0; i < statts_count; i++)
      attachments |= BITFIELD_BIT(statts[i]);
   bool front_only = attachments & ST_ATTACHMENT_FRONT_LEFT_MASK && !(attachments & ST_ATTACHMENT_BACK_LEFT_MASK);

   for (i = 0; i < statts_count; i++) {
      enum pipe_format format;
      unsigned bind;

      dri_drawable_get_format(drawable, statts[i], &format, &bind);
      templ.format = format;

      /* the texture already exists or not requested */
      if (!drawable->textures[statts[i]]) {
         if (statts[i] == ST_ATTACHMENT_BACK_LEFT ||
             statts[i] == ST_ATTACHMENT_DEPTH_STENCIL ||
             (statts[i] == ST_ATTACHMENT_FRONT_LEFT && front_only))
            bind |= PIPE_BIND_DISPLAY_TARGET;

         if (format == PIPE_FORMAT_NONE)
            continue;

         templ.bind = bind;
         templ.nr_samples = 0;
         templ.nr_storage_samples = 0;

         if (statts[i] < ST_ATTACHMENT_DEPTH_STENCIL && is_window) {
            void *data;
            if (statts[i] == ST_ATTACHMENT_BACK_LEFT || (statts[i] == ST_ATTACHMENT_FRONT_LEFT && front_only))
               data = &cdraw->info;
            else
               data = drawable->textures[ST_ATTACHMENT_BACK_LEFT];
            assert(data);
            drawable->textures[statts[i]] =
               screen->base.screen->resource_create_drawable(screen->base.screen, &templ, data);
         }
#ifdef VK_USE_PLATFORM_XCB_KHR
         else if (is_pixmap && statts[i] == ST_ATTACHMENT_FRONT_LEFT && !kscreen->is_sw) {
            drawable->textures[statts[i]] = kopper_get_pixmap_buffer(cdraw, format);
            handle_in_fence(ctx->cPriv, cdraw->image);
         }
#endif
         else {
            drawable->textures[statts[i]] =
               screen->base.screen->resource_create(screen->base.screen, &templ);
         }
      }
      if (drawable->stvis.samples > 1 && !drawable->msaa_textures[statts[i]]) {
         templ.bind = bind &
            ~(PIPE_BIND_SCANOUT | PIPE_BIND_SHARED | PIPE_BIND_DISPLAY_TARGET);
         templ.nr_samples = drawable->stvis.samples;
         templ.nr_storage_samples = drawable->stvis.samples;
         drawable->msaa_textures[statts[i]] =
            screen->base.screen->resource_create(screen->base.screen, &templ);

         dri_pipe_blit(ctx->st->pipe,
                       drawable->msaa_textures[statts[i]],
                       drawable->textures[statts[i]]);
      }
   }
}

static inline void
get_drawable_info(__DRIdrawable *dPriv, int *x, int *y, int *w, int *h)
{
   __DRIscreen *sPriv = dPriv->driScreenPriv;
   const __DRIswrastLoaderExtension *loader = sPriv->swrast_loader;

   if (loader)
      loader->getDrawableInfo(dPriv,
                              x, y, w, h,
                              dPriv->loaderPrivate);
}

static void
kopper_update_drawable_info(struct dri_drawable *drawable)
{
   __DRIdrawable *dPriv = drawable->dPriv;
   __DRIscreen *sPriv = dPriv->driScreenPriv;
   struct kopper_drawable *cdraw = (struct kopper_drawable *)drawable;
   bool is_window = cdraw->info.bos.sType != 0;
   int x, y;
   struct kopper_screen *kscreen = (struct kopper_screen*)sPriv->driverPrivate;
   struct pipe_screen *screen = kscreen->screen;
   struct pipe_resource *ptex = drawable->textures[ST_ATTACHMENT_BACK_LEFT] ?
                                drawable->textures[ST_ATTACHMENT_BACK_LEFT] :
                                drawable->textures[ST_ATTACHMENT_FRONT_LEFT];

   bool do_kopper_update = is_window && ptex && kscreen->base.fd == -1;
   if (cdraw->info.bos.sType == VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR && do_kopper_update)
      zink_kopper_update(screen, ptex, &dPriv->w, &dPriv->h);
   else
      get_drawable_info(dPriv, &x, &y, &dPriv->w, &dPriv->h);
}

static inline void
kopper_present_texture(struct pipe_context *pipe, __DRIdrawable *dPriv,
                      struct pipe_resource *ptex, struct pipe_box *sub_box)
{
   struct dri_drawable *drawable = dri_drawable(dPriv);
   struct dri_screen *screen = dri_screen(drawable->sPriv);

   screen->base.screen->flush_frontbuffer(screen->base.screen, pipe, ptex, 0, 0, drawable, sub_box);
}

static inline void
kopper_copy_to_front(struct pipe_context *pipe,
                    __DRIdrawable * dPriv,
                    struct pipe_resource *ptex)
{
   kopper_present_texture(pipe, dPriv, ptex, NULL);

   kopper_invalidate_drawable(dPriv);
}

static bool
kopper_flush_frontbuffer(struct dri_context *ctx,
                         struct dri_drawable *drawable,
                         enum st_attachment_type statt)
{
   struct pipe_resource *ptex;

   if (!ctx || statt != ST_ATTACHMENT_FRONT_LEFT)
      return false;

   /* Wait for glthread to finish because we can't use pipe_context from
    * multiple threads.
    */
   if (ctx->st->thread_finish)
      ctx->st->thread_finish(ctx->st);

   if (drawable) {
      /* prevent recursion */
      if (drawable->flushing)
         return true;

      drawable->flushing = true;
   }

   if (drawable->stvis.samples > 1) {
      /* Resolve the front buffer. */
      dri_pipe_blit(ctx->st->pipe,
                    drawable->textures[ST_ATTACHMENT_FRONT_LEFT],
                    drawable->msaa_textures[ST_ATTACHMENT_FRONT_LEFT]);
   }
   ptex = drawable->textures[statt];

   if (ptex) {
      ctx->st->pipe->flush_resource(ctx->st->pipe, drawable->textures[ST_ATTACHMENT_FRONT_LEFT]);
      struct pipe_screen *screen = drawable->screen->base.screen;
      struct st_context_iface *st;
      struct pipe_fence_handle *new_fence = NULL;
      st = ctx->st;
      if (st->thread_finish)
         st->thread_finish(st);

      st->flush(st, ST_FLUSH_FRONT, &new_fence, NULL, NULL);
      if (drawable) {
         drawable->flushing = false;
      }
      /* throttle on the previous fence */
      if (drawable->throttle_fence) {
         screen->fence_finish(screen, NULL, drawable->throttle_fence, PIPE_TIMEOUT_INFINITE);
         screen->fence_reference(screen, &drawable->throttle_fence, NULL);
      }
      drawable->throttle_fence = new_fence;
      kopper_copy_to_front(st->pipe, ctx->dPriv, ptex);
   }

   return true;
}

static inline void
get_image(__DRIdrawable *dPriv, int x, int y, int width, int height, void *data)
{
   __DRIscreen *sPriv = dPriv->driScreenPriv;
   const __DRIswrastLoaderExtension *loader = sPriv->swrast_loader;

   loader->getImage(dPriv,
                    x, y, width, height,
                    data, dPriv->loaderPrivate);
}

static inline bool
get_image_shm(__DRIdrawable *dPriv, int x, int y, int width, int height,
              struct pipe_resource *res)
{
   __DRIscreen *sPriv = dPriv->driScreenPriv;
   const __DRIswrastLoaderExtension *loader = sPriv->swrast_loader;
   struct winsys_handle whandle;

   whandle.type = WINSYS_HANDLE_TYPE_SHMID;

   if (loader->base.version < 4 || !loader->getImageShm)
      return FALSE;

   if (!res->screen->resource_get_handle(res->screen, NULL, res, &whandle, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE))
      return FALSE;

   if (loader->base.version > 5 && loader->getImageShm2)
      return loader->getImageShm2(dPriv, x, y, width, height, whandle.handle, dPriv->loaderPrivate);

   loader->getImageShm(dPriv, x, y, width, height, whandle.handle, dPriv->loaderPrivate);
   return TRUE;
}

static void
kopper_update_tex_buffer(struct dri_drawable *drawable,
                         struct dri_context *ctx,
                         struct pipe_resource *res)
{
   __DRIdrawable *dPriv = drawable->dPriv;
   __DRIscreen *sPriv = dPriv->driScreenPriv;
   struct kopper_screen *kscreen = (struct kopper_screen*)sPriv->driverPrivate;
   struct kopper_drawable *cdraw = (struct kopper_drawable *)drawable;
   struct st_context *st_ctx = (struct st_context *)ctx->st;
   struct pipe_context *pipe = st_ctx->pipe;
   struct pipe_transfer *transfer;
   char *map;
   int x, y, w, h;
   int ximage_stride, line;
   if (kscreen->has_dmabuf || cdraw->is_window || cdraw->info.bos.sType != VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR)
      return;
   int cpp = util_format_get_blocksize(res->format);

   /* Wait for glthread to finish because we can't use pipe_context from
    * multiple threads.
    */
   if (ctx->st->thread_finish)
      ctx->st->thread_finish(ctx->st);

   get_drawable_info(dPriv, &x, &y, &w, &h);

   map = pipe_texture_map(pipe, res,
                          0, 0, // level, layer,
                          PIPE_MAP_WRITE,
                          x, y, w, h, &transfer);

   /* Copy the Drawable content to the mapped texture buffer */
   if (!get_image_shm(dPriv, x, y, w, h, res))
      get_image(dPriv, x, y, w, h, map);

   /* The pipe transfer has a pitch rounded up to the nearest 64 pixels.
      get_image() has a pitch rounded up to 4 bytes.  */
   ximage_stride = ((w * cpp) + 3) & -4;
   for (line = h-1; line; --line) {
      memmove(&map[line * transfer->stride],
              &map[line * ximage_stride],
              ximage_stride);
   }

   pipe_texture_unmap(pipe, transfer);
}

static void
kopper_flush_swapbuffers(struct dri_context *ctx,
                         struct dri_drawable *drawable)
{
   /* does this actually need to do anything? */
}

// XXX this frees its second argument as a side effect - regardless of success
// - since the point is to use it as the superclass initializer before we add
// our own state. kindagross but easier than fixing the object model first.
static struct kopper_drawable *
kopper_create_drawable(__DRIdrawable *dPriv, struct dri_drawable *base)
{
   struct kopper_drawable *_ret = CALLOC_STRUCT(kopper_drawable);

   if (!_ret)
      goto out;
   struct dri_drawable *ret = &_ret->base;

   // copy all the elements
   *ret = *base;

   // relocate references to the old struct
   ret->base.visual = &ret->stvis;
   ret->base.st_manager_private = (void *) ret;
   dPriv->driverPrivate = ret;

   // and fill in the vtable
   ret->allocate_textures = kopper_allocate_textures;
   ret->update_drawable_info = kopper_update_drawable_info;
   ret->flush_frontbuffer = kopper_flush_frontbuffer;
   ret->update_tex_buffer = kopper_update_tex_buffer;
   ret->flush_swapbuffers = kopper_flush_swapbuffers;

out:
   free(base);
   return _ret;
}

static boolean
kopper_create_buffer(__DRIscreen * sPriv,
                     __DRIdrawable * dPriv,
                     const struct gl_config *visual, boolean isPixmap)
{
   struct kopper_drawable *drawable = NULL;

   /* always pass !pixmap because it isn't "handled" or relevant */
   if (!dri_create_buffer(sPriv, dPriv, visual, false))
      return FALSE;

   drawable = kopper_create_drawable(dPriv, dPriv->driverPrivate);
   if (!drawable)
      return FALSE;

   drawable->info.has_alpha = visual->alphaBits > 0;
   if (sPriv->kopper_loader->SetSurfaceCreateInfo)
      sPriv->kopper_loader->SetSurfaceCreateInfo(dPriv->loaderPrivate,
                                                 &drawable->info);
   drawable->is_window = !isPixmap && drawable->info.bos.sType != 0;

   return TRUE;
}

static int64_t
kopperSwapBuffers(__DRIdrawable *dPriv)
{
   struct dri_context *ctx = dri_get_current(dPriv->driScreenPriv);
   struct dri_drawable *drawable = dri_drawable(dPriv);
   struct kopper_drawable *kdraw = (struct kopper_drawable *)drawable;
   struct pipe_resource *ptex;

   if (!ctx)
      return 0;

   ptex = drawable->textures[ST_ATTACHMENT_BACK_LEFT];
   if (!ptex)
      return 0;

   /* Wait for glthread to finish because we can't use pipe_context from
    * multiple threads.
    */
   if (ctx->st->thread_finish)
      ctx->st->thread_finish(ctx->st);

   drawable->texture_stamp = dPriv->lastStamp - 1;
   dri_flush(ctx->cPriv, dPriv, __DRI2_FLUSH_DRAWABLE | __DRI2_FLUSH_CONTEXT, __DRI2_THROTTLE_SWAPBUFFER);
   kopper_copy_to_front(ctx->st->pipe, dPriv, ptex);
   if (kdraw->is_window && !zink_kopper_check(ptex))
      return -1;
   if (!drawable->textures[ST_ATTACHMENT_FRONT_LEFT]) {
      return 0;
   }

   /* have to manually swap the pointers here to make frontbuffer readback work */
   drawable->textures[ST_ATTACHMENT_BACK_LEFT] = drawable->textures[ST_ATTACHMENT_FRONT_LEFT];
   drawable->textures[ST_ATTACHMENT_FRONT_LEFT] = ptex;

   return 0;
}

static void
kopper_swap_buffers(__DRIdrawable *dPriv)
{
   kopperSwapBuffers(dPriv);
}

static __DRIdrawable *
kopperCreateNewDrawable(__DRIscreen *screen,
                        const __DRIconfig *config,
                        void *data,
                        int is_pixmap)
{
    __DRIdrawable *pdraw;

    assert(data != NULL);

    pdraw = malloc(sizeof *pdraw);
    if (!pdraw)
	return NULL;

    pdraw->loaderPrivate = data;

    pdraw->driScreenPriv = screen;
    pdraw->driContextPriv = NULL;
    pdraw->refcount = 0;
    pdraw->lastStamp = 0;
    pdraw->w = 0;
    pdraw->h = 0;

    //dri_get_drawable(pdraw);
    pdraw->refcount++;

    if (!screen->driver->CreateBuffer(screen, pdraw, &config->modes,
                                      is_pixmap)) {
       free(pdraw);
       return NULL;
    }

    pdraw->dri2.stamp = pdraw->lastStamp + 1;

    return pdraw;
}

static void
kopperSetSwapInterval(__DRIdrawable *dPriv, int interval)
{
   struct dri_drawable *drawable = dri_drawable(dPriv);
   struct kopper_drawable *cdraw = (struct kopper_drawable *)drawable;
   struct dri_screen *screen = dri_screen(drawable->sPriv);
   struct kopper_screen *kscreen = (struct kopper_screen *)screen;
   struct pipe_screen *pscreen = kscreen->screen;
   struct pipe_resource *ptex = drawable->textures[ST_ATTACHMENT_BACK_LEFT] ?
                                drawable->textures[ST_ATTACHMENT_BACK_LEFT] :
                                drawable->textures[ST_ATTACHMENT_FRONT_LEFT];

   /* the conditional is because we can be called before buffer allocation.  If
    * we're before allocation, then the initial_swap_interval will be used when
    * the swapchain is eventually created.
    */
   if (ptex)
      zink_kopper_set_swap_interval(pscreen, ptex, interval);
   cdraw->info.initial_swap_interval = interval;
}

static int
kopperQueryBufferAge(__DRIdrawable *dPriv)
{
   struct dri_context *ctx = dri_get_current(dPriv->driScreenPriv);
   struct dri_drawable *drawable = dri_drawable(dPriv);
   struct pipe_resource *ptex = drawable->textures[ST_ATTACHMENT_BACK_LEFT] ?
                                drawable->textures[ST_ATTACHMENT_BACK_LEFT] :
                                drawable->textures[ST_ATTACHMENT_FRONT_LEFT];

   /* Wait for glthread to finish because we can't use pipe_context from
    * multiple threads.
    */
   if (ctx->st->thread_finish)
      ctx->st->thread_finish(ctx->st);

   return zink_kopper_query_buffer_age(ctx->st->pipe, ptex);
}

const __DRIkopperExtension driKopperExtension = {
   .base = { __DRI_KOPPER, 1 },
   .createNewDrawable          = kopperCreateNewDrawable,
   .swapBuffers                = kopperSwapBuffers,
   .setSwapInterval            = kopperSetSwapInterval,
   .queryBufferAge             = kopperQueryBufferAge,
};

const struct __DriverAPIRec galliumvk_driver_api = {
   .InitScreen = kopper_init_screen,
   .DestroyScreen = dri_destroy_screen,
   .CreateBuffer = kopper_create_buffer,
   .DestroyBuffer = dri_destroy_buffer,
   .SwapBuffers = kopper_swap_buffers,
   .CopySubBuffer = NULL,
};

static const struct __DRIDriverVtableExtensionRec galliumvk_vtable = {
   .base = { __DRI_DRIVER_VTABLE, 1 },
   .vtable = &galliumvk_driver_api,
};

const __DRIextension *galliumvk_driver_extensions[] = {
   &driCoreExtension.base,
   &driSWRastExtension.base,
   &driDRI2Extension.base,
   &driImageDriverExtension.base,
   &driKopperExtension.base,
   &gallium_config_options.base,
   &galliumvk_vtable.base,
   NULL
};

/* vim: set sw=3 ts=8 sts=3 expandtab: */