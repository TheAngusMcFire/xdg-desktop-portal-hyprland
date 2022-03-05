#include "xdpw.h"
#include "screencast_common.h"
#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libdrm/drm_fourcc.h>
#include <xf86drm.h>
#include "linux-dmabuf-unstable-v1-client-protocol.h"

#include "logger.h"

void randname(char *buf) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long r = ts.tv_nsec;
	for (int i = 0; i < 6; ++i) {
		assert(buf[i] == 'X');
		buf[i] = 'A'+(r&15)+(r&16)*2;
		r >>= 5;
	}
}

static char *gbm_find_render_node() {
	drmDevice *devices[64];
	char *render_node = NULL;

	int n = drmGetDevices2(0, devices, sizeof(devices) / sizeof(devices[0]));
	for (int i = 0; i < n; ++i) {
		drmDevice *dev = devices[i];
		if (!(dev->available_nodes & (1 << DRM_NODE_RENDER)))
			continue;

		render_node = strdup(dev->nodes[DRM_NODE_RENDER]);
		break;
	}

	drmFreeDevices(devices, n);
	return render_node;
}

struct gbm_device *xdpw_gbm_device_create(void) {
	struct gbm_device *gbm;
	char *render_node = NULL;

	render_node = gbm_find_render_node();
	if (render_node == NULL) {
		logprint(ERROR, "xdpw: Could not find render node");
		return NULL;
	}
	logprint(INFO, "xdpw: Using render node %s", render_node);

	int fd = open(render_node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		logprint(ERROR, "xdpw: Could not open render node %s", render_node);
		free(render_node);
		return NULL;
	}

	free(render_node);
	gbm = gbm_create_device(fd);
	return gbm;
}

static int anonymous_shm_open(void) {
	char name[] = "/xdpw-shm-XXXXXX";
	int retries = 100;

	do {
		randname(name + strlen(name) - 6);

		--retries;
		// shm_open guarantees that O_CLOEXEC is set
		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
		if (fd >= 0) {
			shm_unlink(name);
			return fd;
		}
	} while (retries > 0 && errno == EEXIST);

	return -1;
}

static struct wl_buffer *import_wl_shm_buffer(struct xdpw_screencast_instance *cast, int fd,
		enum wl_shm_format fmt, int width, int height, int stride) {
	struct xdpw_screencast_context *ctx = cast->ctx;
	int size = stride * height;

	if (fd < 0) {
		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(ctx->shm, fd, size);
	struct wl_buffer *buffer =
		wl_shm_pool_create_buffer(pool, 0, width, height, stride, fmt);
	wl_shm_pool_destroy(pool);

	return buffer;
}

struct xdpw_buffer *xdpw_buffer_create(struct xdpw_screencast_instance *cast,
		enum buffer_type buffer_type, struct xdpw_screencopy_frame_info *frame_info) {
	struct xdpw_buffer *buffer = calloc(1, sizeof(struct xdpw_buffer));
	buffer->width = frame_info->width;
	buffer->height = frame_info->height;
	buffer->format = frame_info->format;
	buffer->buffer_type = buffer_type;

	switch (buffer_type) {
	case WL_SHM:
		buffer->size = frame_info->size;
		buffer->stride = frame_info->stride;
		buffer->offset = 0;
		buffer->fd = anonymous_shm_open();
		if (buffer->fd == -1) {
			logprint(ERROR, "xdpw: unable to create anonymous filedescriptor");
			free(buffer);
			return NULL;
		}

		if (ftruncate(buffer->fd, buffer->size) < 0) {
			logprint(ERROR, "xdpw: unable to truncate filedescriptor");
			close(buffer->fd);
			free(buffer);
			return NULL;
		}

		buffer->buffer = import_wl_shm_buffer(cast, buffer->fd, xdpw_format_wl_shm_from_drm_fourcc(frame_info->format),
				frame_info->width, frame_info->height, frame_info->stride);
		if (buffer->buffer == NULL) {
			logprint(ERROR, "xdpw: unable to create wl_buffer");
			close(buffer->fd);
			free(buffer);
			return NULL;
		}
		break;
	case DMABUF:;
		uint32_t flags = GBM_BO_USE_RENDERING;
		if (cast->ctx->state->config->screencast_conf.force_mod_linear) {
			flags |= GBM_BO_USE_LINEAR;
		}

		buffer->bo = gbm_bo_create(cast->ctx->gbm, frame_info->width, frame_info->height,
				frame_info->format, flags);
		if (buffer->bo == NULL) {
			logprint(ERROR, "xdpw: failed to create gbm_bo");
			free(buffer);
			return NULL;
		}

		struct zwp_linux_buffer_params_v1 *params;
		params = zwp_linux_dmabuf_v1_create_params(cast->ctx->linux_dmabuf);
		if (!params) {
			logprint(ERROR, "xdpw: failed to create linux_buffer_params");
			gbm_bo_destroy(buffer->bo);
			free(buffer);
			return NULL;
		}

		buffer->size = 0;
		buffer->stride = gbm_bo_get_stride(buffer->bo);
		buffer->offset = gbm_bo_get_offset(buffer->bo, 0);
		uint64_t mod = gbm_bo_get_modifier(buffer->bo);
		buffer->fd = gbm_bo_get_fd(buffer->bo);

		if (buffer->fd < 0) {
			logprint(ERROR, "xdpw: failed to get file descriptor");
			zwp_linux_buffer_params_v1_destroy(params);
			gbm_bo_destroy(buffer->bo);
			free(buffer);
			return NULL;
		}

		zwp_linux_buffer_params_v1_add(params, buffer->fd, 0, buffer->offset, buffer->stride,
			mod >> 32, mod & 0xffffffff);
		buffer->buffer = zwp_linux_buffer_params_v1_create_immed(params,
			buffer->width, buffer->height,
			buffer->format, /* flags */ 0);
		zwp_linux_buffer_params_v1_destroy(params);

		if (!buffer->buffer) {
			logprint(ERROR, "xdpw: failed to create buffer");
			gbm_bo_destroy(buffer->bo);
			close(buffer->fd);
			free(buffer);
			return NULL;
		}
	}

	return buffer;
}

void xdpw_buffer_destroy(struct xdpw_buffer *buffer) {
	wl_buffer_destroy(buffer->buffer);
	if (buffer->buffer_type == DMABUF) {
		gbm_bo_destroy(buffer->bo);
	}
	close(buffer->fd);
	wl_list_remove(&buffer->link);
	free(buffer);
}

enum wl_shm_format xdpw_format_wl_shm_from_drm_fourcc(uint32_t format) {
	switch (format) {
	case DRM_FORMAT_ARGB8888:
		return WL_SHM_FORMAT_ARGB8888;
	case DRM_FORMAT_XRGB8888:
		return WL_SHM_FORMAT_XRGB8888;
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_RGBX1010102:
	case DRM_FORMAT_BGRX1010102:
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_ABGR2101010:
	case DRM_FORMAT_RGBA1010102:
	case DRM_FORMAT_BGRA1010102:
		return (enum wl_shm_format)format;
	default:
		logprint(ERROR, "xdg-desktop-portal-wlr: unsupported drm "
			"format 0x%08x", format);
		abort();
	}
}

uint32_t xdpw_format_drm_fourcc_from_wl_shm(enum wl_shm_format format) {
	switch (format) {
	case WL_SHM_FORMAT_ARGB8888:
		return DRM_FORMAT_ARGB8888;
	case WL_SHM_FORMAT_XRGB8888:
		return DRM_FORMAT_XRGB8888;
	case WL_SHM_FORMAT_RGBA8888:
	case WL_SHM_FORMAT_RGBX8888:
	case WL_SHM_FORMAT_ABGR8888:
	case WL_SHM_FORMAT_XBGR8888:
	case WL_SHM_FORMAT_BGRA8888:
	case WL_SHM_FORMAT_BGRX8888:
	case WL_SHM_FORMAT_NV12:
	case WL_SHM_FORMAT_XRGB2101010:
	case WL_SHM_FORMAT_XBGR2101010:
	case WL_SHM_FORMAT_RGBX1010102:
	case WL_SHM_FORMAT_BGRX1010102:
	case WL_SHM_FORMAT_ARGB2101010:
	case WL_SHM_FORMAT_ABGR2101010:
	case WL_SHM_FORMAT_RGBA1010102:
	case WL_SHM_FORMAT_BGRA1010102:
		return (uint32_t)format;
	default:
		logprint(ERROR, "xdg-desktop-portal-wlr: unsupported wl_shm "
			"format 0x%08x", format);
		abort();
	}
}

enum spa_video_format xdpw_format_pw_from_drm_fourcc(uint32_t format) {
	switch (format) {
	case DRM_FORMAT_ARGB8888:
		return SPA_VIDEO_FORMAT_BGRA;
	case DRM_FORMAT_XRGB8888:
		return SPA_VIDEO_FORMAT_BGRx;
	case DRM_FORMAT_RGBA8888:
		return SPA_VIDEO_FORMAT_ABGR;
	case DRM_FORMAT_RGBX8888:
		return SPA_VIDEO_FORMAT_xBGR;
	case DRM_FORMAT_ABGR8888:
		return SPA_VIDEO_FORMAT_RGBA;
	case DRM_FORMAT_XBGR8888:
		return SPA_VIDEO_FORMAT_RGBx;
	case DRM_FORMAT_BGRA8888:
		return SPA_VIDEO_FORMAT_ARGB;
	case DRM_FORMAT_BGRX8888:
		return SPA_VIDEO_FORMAT_xRGB;
	case DRM_FORMAT_NV12:
		return SPA_VIDEO_FORMAT_NV12;
	case DRM_FORMAT_XRGB2101010:
		return SPA_VIDEO_FORMAT_xRGB_210LE;
	case DRM_FORMAT_XBGR2101010:
		return SPA_VIDEO_FORMAT_xBGR_210LE;
	case DRM_FORMAT_RGBX1010102:
		return SPA_VIDEO_FORMAT_RGBx_102LE;
	case DRM_FORMAT_BGRX1010102:
		return SPA_VIDEO_FORMAT_BGRx_102LE;
	case DRM_FORMAT_ARGB2101010:
		return SPA_VIDEO_FORMAT_ARGB_210LE;
	case DRM_FORMAT_ABGR2101010:
		return SPA_VIDEO_FORMAT_ABGR_210LE;
	case DRM_FORMAT_RGBA1010102:
		return SPA_VIDEO_FORMAT_RGBA_102LE;
	case DRM_FORMAT_BGRA1010102:
		return SPA_VIDEO_FORMAT_BGRA_102LE;
	default:
		logprint(ERROR, "xdg-desktop-portal-wlr: failed to convert drm "
			"format 0x%08x to spa_video_format", format);
		abort();
	}
}

enum spa_video_format xdpw_format_pw_strip_alpha(enum spa_video_format format) {
	switch (format) {
	case SPA_VIDEO_FORMAT_BGRA:
		return SPA_VIDEO_FORMAT_BGRx;
	case SPA_VIDEO_FORMAT_ABGR:
		return SPA_VIDEO_FORMAT_xBGR;
	case SPA_VIDEO_FORMAT_RGBA:
		return SPA_VIDEO_FORMAT_RGBx;
	case SPA_VIDEO_FORMAT_ARGB:
		return SPA_VIDEO_FORMAT_xRGB;
	case SPA_VIDEO_FORMAT_ARGB_210LE:
		return SPA_VIDEO_FORMAT_xRGB_210LE;
	case SPA_VIDEO_FORMAT_ABGR_210LE:
		return SPA_VIDEO_FORMAT_xBGR_210LE;
	case SPA_VIDEO_FORMAT_RGBA_102LE:
		return SPA_VIDEO_FORMAT_RGBx_102LE;
	case SPA_VIDEO_FORMAT_BGRA_102LE:
		return SPA_VIDEO_FORMAT_BGRx_102LE;
	default:
		return SPA_VIDEO_FORMAT_UNKNOWN;
	}
}

enum xdpw_chooser_types get_chooser_type(const char *chooser_type) {
	if (!chooser_type || strcmp(chooser_type, "default") == 0) {
		return XDPW_CHOOSER_DEFAULT;
	} else if (strcmp(chooser_type, "none") == 0) {
		return XDPW_CHOOSER_NONE;
	} else if (strcmp(chooser_type, "simple") == 0) {
		return XDPW_CHOOSER_SIMPLE;
	} else if (strcmp(chooser_type, "dmenu") == 0) {
		return XDPW_CHOOSER_DMENU;
	}
	fprintf(stderr, "Could not understand chooser type %s\n", chooser_type);
	exit(1);
}

const char *chooser_type_str(enum xdpw_chooser_types chooser_type) {
	switch (chooser_type) {
	case XDPW_CHOOSER_DEFAULT:
		return "default";
	case XDPW_CHOOSER_NONE:
		return "none";
	case XDPW_CHOOSER_SIMPLE:
		return "simple";
	case XDPW_CHOOSER_DMENU:
		return "dmenu";
	}
	fprintf(stderr, "Could not find chooser type %d\n", chooser_type);
	abort();
}
