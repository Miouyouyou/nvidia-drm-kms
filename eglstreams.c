// gcc -o drmgl Linux_DRM_OpenGLES.c `pkg-config --cflags --libs libdrm` -lgbm -lEGL -lGLESv2

/*
 * Copyright (c) 2012 Arvin Schnell <arvin.schnell@gmail.com>
 * Copyright (c) 2012 Rob Clark <rob@ti.com>
 * Copyright (c) 2017 Miouyouyou <Myy> <myy@miouyouyou.fr>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* Based on a egl cube test app originally written by Arvin Schnell */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>   // errno
#include <stdbool.h> // bool
#include <unistd.h>  // close

#include <sys/mman.h> // mmap

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdrm/drm_mode.h> // DRM_MODE_XXX

#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <assert.h>

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define LOGF(fmt, ...) \
	fprintf(\
		stderr,\
		 "[%s:%s:%d] " fmt "\n",\
		 __FILE__, __func__, __LINE__, ##__VA_ARGS__)

#define LOGVF(fmt, ...) fprintf(stdout, fmt "\n", ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#define LOG_EGL_ERROR(fmt, ...) do {\
	fprintf(stderr, fmt "\n", ##__VA_ARGS__); \
} while(0)

struct myy_opengl_infos {
	EGLDisplay display;
	EGLConfig config;
	EGLContext context;
	EGLSurface surface;
};
typedef struct myy_opengl_infos myy_opengl_infos_t;

struct myy_drm_infos {
	int fd;
	drmModeModeInfo mode;
	uint32_t crtc_id;
	uint32_t plane_id;
	uint32_t connector_id;
	uint32_t width;
	uint32_t height;
	uint32_t framebuffer_id;
	uint8_t * __restrict framebuffer;
};
typedef struct myy_drm_infos myy_drm_infos_t;

struct drm_fb {
	uint32_t fb_id;
};

struct myy_nvidia_functions {
	PFNEGLQUERYDEVICESEXTPROC eglQueryDevices;
	PFNEGLQUERYDEVICESTRINGEXTPROC eglQueryDeviceString;
	PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplay;
	PFNEGLGETOUTPUTLAYERSEXTPROC eglGetOutputLayers;
	PFNEGLCREATESTREAMKHRPROC eglCreateStream;
	PFNEGLSTREAMCONSUMEROUTPUTEXTPROC eglStreamConsumerOutput;
	PFNEGLCREATESTREAMPRODUCERSURFACEKHRPROC eglCreateStreamProducerSurface;
};

struct myy_drm_caps {
	char const * __restrict const name;
	int const cap_code;
	int const cap_arg;
};

static void myy_drm_config_dump(
	myy_drm_infos_t * __restrict const myy_drm_conf)
{
	LOGF("[Current DRM config]\n"
		"\tfd             = %d\n"
		"\tcrtc_id        = %u\n"
		"\tplane_id       = %u\n"
		"\tconnector_id   = %u\n"
		"\twidth          = %d\n"
		"\theight         = %d\n"
		"\tframebuffer_id = %d\n",
		myy_drm_conf->fd          ,
		myy_drm_conf->crtc_id     ,
		myy_drm_conf->plane_id    ,
		myy_drm_conf->connector_id,
		myy_drm_conf->width,
		myy_drm_conf->height,
		myy_drm_conf->framebuffer_id);
}

/* Ugh... yeah... How about eglCheckForExtension("name", TYPE) ?
 * Anyway, eglQueryString will return a space-separated list of
 * supported extensions on the object passed :
 * 
 *     EGL_EXT_Blablabla EGL_EXT_Nyanyanya EGL_EXT_Doubidoubidou
 * 
 * If you want to check that your extension is supported, you'll
 * have to do a string search (not kidding) on the list.
 * 
 * This... is our strstr implementation, that gives nice error
 * messages and check for false positives.
 */
int egl_strstr(
	char const * __restrict const extensions_list,
	char const * __restrict const * __restrict const extensions,
	char const * __restrict const extension_type)
{
	int ret = 0;

	char const * __restrict const * __restrict cursor =
		extensions;

	/* Let's be clear, if your EGL driver return non 0
	 * terminated strings, it's time to terminate your
	 * EGL driver, or the people who wrote them.
	 * If you're concerned about your security, you should
	 * rewrite this entire codebase anyway.
	 */
	char const * __restrict const list_end =
		extensions_list + strlen(extensions_list);

	printf("Supported extensions on %s :\n%s\n",
		extension_type, extensions_list);

	while(*cursor != 0) {
		char const * __restrict const ext_name =
			*cursor;
		size_t const ext_name_length = strlen(ext_name);

		/* Search for the character address where the extension
		 * name starts, in the long list returned by EGL.
		 */
		char const * __restrict const pos =
			strstr(extensions_list, ext_name);

		bool const extension_might_be_found = 
			(pos != NULL) /* strstr returns NULL on failure */
			& ((pos+ext_name_length) <= list_end); /* OOB */
		bool extension_found = false;

		/* Check for false positives like
		 * 'EGL_EXT_device_baseless_unit' instead of
		 * 'EGL_EXT_device_base' for example.
		 */
		if (extension_might_be_found) {
			char const * __restrict const after_ext_name =
				pos+ext_name_length;
			char const after_char = *after_ext_name;
			extension_found =
				(after_char == ' ') | (after_char == '\0');
		}

		if (!extension_found) {
			ret = -1;
			/* We'll continue to check other extensions
			 * in order to alert the user of EVERY SINGLE
			 * extension he needs on his client.
			 */
			fprintf(
				stderr,
				"EGL Client extension %s not found !\n",
				ext_name);
		}

		cursor++;
	}

	return ret;
}

static void egl_print_config_attribs(
	EGLDisplay egl_display,
	EGLConfig const config)
{
	EGLint value;
	eglGetConfigAttrib(egl_display, config, EGL_ALPHA_SIZE, &value);
	LOGF("\tEGL_ALPHA_SIZE : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_BIND_TO_TEXTURE_RGB, &value);
	LOGF("\tEGL_BIND_TO_TEXTURE_RGB : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_BIND_TO_TEXTURE_RGBA, &value);
	LOGF("\tEGL_BIND_TO_TEXTURE_RGBA : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_BLUE_SIZE, &value);
	LOGF("\tEGL_BLUE_SIZE : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_BUFFER_SIZE, &value);
	LOGF("\tEGL_BUFFER_SIZE : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_CONFIG_CAVEAT, &value);
	LOGF("\tEGL_CONFIG_CAVEAT : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_CONFIG_ID, &value);
	LOGF("\tEGL_CONFIG_ID : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_DEPTH_SIZE, &value);
	LOGF("\tEGL_DEPTH_SIZE : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_GREEN_SIZE, &value);
	LOGF("\tEGL_GREEN_SIZE : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_LEVEL, &value);
	LOGF("\tEGL_LEVEL : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_MAX_PBUFFER_WIDTH, &value);
	LOGF("\tEGL_MAX_PBUFFER_WIDTH : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_MAX_PBUFFER_HEIGHT, &value);
	LOGF("\tEGL_MAX_PBUFFER_HEIGHT : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_MAX_PBUFFER_PIXELS, &value);
	LOGF("\tEGL_MAX_PBUFFER_PIXELS : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_MAX_SWAP_INTERVAL, &value);
	LOGF("\tEGL_MAX_SWAP_INTERVAL : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_MIN_SWAP_INTERVAL, &value);
	LOGF("\tEGL_MIN_SWAP_INTERVAL : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_NATIVE_RENDERABLE, &value);
	LOGF("\tEGL_NATIVE_RENDERABLE : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_NATIVE_VISUAL_ID, &value);
	LOGF("\tEGL_NATIVE_VISUAL_ID : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_NATIVE_VISUAL_TYPE, &value);
	LOGF("\tEGL_NATIVE_VISUAL_TYPE : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_RED_SIZE, &value);
	LOGF("\tEGL_RED_SIZE : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_SAMPLE_BUFFERS, &value);
	LOGF("\tEGL_SAMPLE_BUFFERS : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_SAMPLES, &value);
	LOGF("\tEGL_SAMPLES : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_STENCIL_SIZE, &value);
	LOGF("\tEGL_STENCIL_SIZE : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_SURFACE_TYPE, &value);
	LOGF("\tEGL_SURFACE_TYPE : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_TRANSPARENT_TYPE, &value);
	LOGF("\tEGL_TRANSPARENT_TYPE : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_TRANSPARENT_RED_VALUE, &value);
	LOGF("\tEGL_TRANSPARENT_RED_VALUE : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_TRANSPARENT_GREEN_VALUE, &value);
	LOGF("\tEGL_TRANSPARENT_GREEN_VALUE : %d", value);
	eglGetConfigAttrib(egl_display, config, EGL_TRANSPARENT_BLUE_VALUE, &value);
	LOGF("\tEGL_TRANSPARENT_BLUE_VALUE : %d", value);
}

#define NO_CRTC_FOUND (0)
static uint32_t drm_encoder_find_crtc(
	drmModeRes const * __restrict const resources,
	drmModeEncoder const * __restrict const encoder,
	uint32_t * __restrict const crtc_index)
{
	/* 0 for "no CRTC found" */
	uint32_t selected_crtc = NO_CRTC_FOUND;
	uint32_t const n_crtcs = resources->count_crtcs;
	for (uint32_t i = 0;
	     (selected_crtc == NO_CRTC_FOUND) & (i < n_crtcs);
	     i++)
	{
		/* possible_crtcs is a bitmask as described here:
		 * https://dvdhrm.wordpress.com/2012/09/13/linux-drm-mode-setting-api
		 */
		uint32_t const crtc_mask = 1 << i;
		uint32_t const possible_crtcs =
			encoder->possible_crtcs;
		if (possible_crtcs & crtc_mask) {
			selected_crtc = resources->crtcs[i];
			*crtc_index = i;
		}
	}

	return selected_crtc;
}

static uint32_t drm_connector_find_crtc(
	drmModeRes const * __restrict const resources,
	drmModeConnector const * __restrict const connector,
	uint32_t * __restrict const crtc_index,
	int const drm_fd)
{
	uint32_t crtc_id = NO_CRTC_FOUND;

	uint32_t const n_encoders =
		connector->count_encoders;
	for (uint32_t i = 0;
	     (crtc_id == NO_CRTC_FOUND) & (i < n_encoders);
	     i++)
	{
		uint32_t const encoder_id = connector->encoders[i];
		drmModeEncoder * __restrict const encoder =
			drmModeGetEncoder(drm_fd, encoder_id);

		if (encoder) {
			crtc_id = drm_encoder_find_crtc(
				resources, encoder, crtc_index);

			drmModeFreeEncoder(encoder);
		}
		else {
			LOGF("... We asked for the encoders, "
				"got a NULL pointer instead");
		}
	}

	return crtc_id;
}

static void drm_mode_display_infos(
	drmModeModeInfo const * __restrict const mode)
{
	LOGF("[DRM Mode Info] {\n"
		"\tuint32_t clock       = %u;\n"
		"\tuint16_t hdisplay    = %u;\n"
		"\tuint16_t hsync_start = %u;\n"
		"\tuint16_t hsync_end   = %u;\n"
		"\tuint16_t htotal      = %u;\n"
		"\tuint16_t hskew       = %u;\n"
		"\tuint16_t vdisplay    = %u;\n"
		"\tuint16_t vsync_start = %u;\n"
		"\tuint16_t vsync_end   = %u;\n"
		"\tuint16_t vtotal      = %u;\n"
		"\tuint16_t vscan       = %u;\n"
		"\tuint32_t vrefresh    = %u;\n"
		"\tuint32_t flags       = %u;\n"
		"\tuint32_t type        = %u;\n"
		"\tchar     name[32]    = %s;\n"
		"};",
		mode->clock      ,
		mode->hdisplay   ,
		mode->hsync_start,
		mode->hsync_end  ,
		mode->htotal     ,
		mode->hskew      ,
		mode->vdisplay   ,
		mode->vsync_start,
		mode->vsync_end  ,
		mode->vtotal     ,
		mode->vscan      ,
		mode->vrefresh   ,
		mode->flags      ,
		mode->type       ,
		mode->name);
}

static bool drm_connector_seems_valid(
	drmModeConnector * __restrict const connector)
{
	return
		((connector->connection == DRM_MODE_CONNECTED)
		& (connector->count_modes > 0)
		& (connector->count_encoders > 0));
}

static drmModeConnector * drm_get_connector(
	int const drm_fd,
	drmModeRes * __restrict const resources)
{
	drmModeConnector * __restrict connector = NULL;
	/* find a connected connector: */
	uint32_t const n_connectors =
		resources->count_connectors;
	for (uint32_t i = 0; i < n_connectors; i++) {
		connector = drmModeGetConnector(
			drm_fd, resources->connectors[i]);
		if (drm_connector_seems_valid(connector)) {
			break;
		}
		else {
			drmModeFreeConnector(connector);
		    connector = NULL;
		}
	}

	if (!connector) {
		/* we could be fancy and listen for hotplug events and wait for
		 * a connector..
		 */
		LOG_ERROR("No connected screens ?\n");
	}

    return connector;
}

static drmModeModeInfo * drm_connect_select_best_resolution(
	drmModeConnector * __restrict const connector)
{
	int preferred_mode_index = -1;
	int highest_res_mode_index = -1;
	drmModeModeInfo * the_chosen_one;

	/* find prefered mode or the highest resolution mode: */
	for (int i = 0, biggest_area = 0; i < connector->count_modes; i++) {
		drmModeModeInfo const * __restrict const current_mode =
			connector->modes+i;

		drm_mode_display_infos(current_mode);

		if (current_mode->type & DRM_MODE_TYPE_PREFERRED) {
			preferred_mode_index = i;
		}

		int const current_area =
			current_mode->hdisplay * current_mode->vdisplay;
		if (current_area > biggest_area) {
			highest_res_mode_index = i;
			biggest_area = current_area;
		}

	}

	if (preferred_mode_index >= 0) {
		the_chosen_one = 
			connector->modes+preferred_mode_index;
	}
	else if (highest_res_mode_index >= 0) {
		the_chosen_one =
			connector->modes+highest_res_mode_index;
	}
	else {
		LOG_ERROR(
			"Wow, a screen with zero resolution available !\n"
			"Now, THAT'S useful !");
		the_chosen_one = NULL;
	}

	return the_chosen_one;
}
	

static int myy_drm_set_caps(
	int const drm_fd,
	struct myy_drm_caps const * __restrict caps)
{
	int ret = 0;
	while(caps->name) {
		if (drmSetClientCap(drm_fd, caps->cap_code, caps->cap_arg)
		    != 0)
		{
			LOG_ERROR("Could not set property %s.\n", caps->name);
			ret = -1;
			/* Keep going, enumerate all issues and provide
			 * meaningful error messages.
			 * Then fail at the end.
			 */
		}
		else {
			LOGF("%s (%d) = %d -> 0\n",
				 caps->name, caps->cap_code, caps->cap_arg);
		}
		caps++;
	}

	return ret;
}

static uint32_t drm_get_best_crtc(
	int const drm_fd,
	drmModeRes * __restrict const resources,
	drmModeConnector * __restrict const connector,
	uint32_t * __restrict const crtc_index)
{

	/* In order to get a valid "Primary plane ID",
	 * which will be used by the NVIDIA EGL Extension
	 * later, we need to get index of the CRTC used
	 * selected.
	 * So no shortcuts.
	 */
	return drm_connector_find_crtc(
		resources, connector, crtc_index, drm_fd);
}

static uint64_t drm_get_property(
	int const drm_fd,
	uint32_t const object_id,
	uint32_t const object_type,
	char const * __restrict const property_name,
	int * __restrict const prop_found)
{
	uint64_t value = 0;
	int found = 0;

	drmModeObjectProperties * __restrict const object_properties =
		drmModeObjectGetProperties(drm_fd, object_id, object_type);
	uint32_t const n_props = object_properties->count_props;

	for (uint32_t i = 0; (found == 0) & (i < n_props); i++) {

		drmModePropertyRes * __restrict const prop =
			drmModeGetProperty(drm_fd, object_properties->props[i]);

		if (prop == NULL) {
			LOGF("[DRM Property] "
				"Property %d on %d led to a NULL Pointer !\n",
				i, n_props);
			break;
		}

		if (strcmp(property_name, prop->name) == 0) {
			value = object_properties->prop_values[i];
			found = 1;
		}

		drmModeFreeProperty(prop);
	}

	drmModeFreeObjectProperties(object_properties);

	if (!found) {
		LOGF("[DRM Property] "
			"Property \"%s\" not found...\n",
			property_name);
	}

	*prop_found = found;
	return value;
}

#define NO_PLANE_FOUND (0)
static uint32_t drm_get_primary_plane_for_crtc(
	int drm_fd,
	uint32_t const selected_crtc_index)
{
	uint32_t plane_id = NO_PLANE_FOUND;
	drmModePlaneRes * __restrict const planes_resources =
		drmModeGetPlaneResources(drm_fd);

	if (planes_resources != NULL) {
		uint32_t const n_planes = planes_resources->count_planes;

		for (uint32_t i = 0;
			(plane_id == NO_PLANE_FOUND) & (i < n_planes);
			i++)
		{
			uint32_t const plane_i =
				planes_resources->planes[i];
			drmModePlane * __restrict const plane =
				drmModeGetPlane(drm_fd, plane_i);

			if (plane == NULL) {
				LOGF("Plane %d leads to a NULL pointer ! WHAT !!?\n",
					i);
				break;
			}

			uint32_t const crtcs = plane->possible_crtcs;
			drmModeFreePlane(plane);

			if ((crtcs & (1 << selected_crtc_index)) == 0) {
				/* This is not the plane you're looking for */
				continue;
			}

			int property_found = 0;
			uint64_t const type = drm_get_property(
				drm_fd,
				plane_i,
				DRM_MODE_OBJECT_PLANE,
				"type",
				&property_found);
			if ((property_found) & (type == DRM_PLANE_TYPE_PRIMARY))
			{
				plane_id = plane_i;
			}
		}

		drmModeFreePlaneResources(planes_resources);
	}
	else {
		LOGF("No planes resources for this DRM node ??\n");
	}

	return plane_id;
}


static int drm_init(
	char const * __restrict const drm_device_file,
	myy_drm_infos_t * __restrict const myy_drm_conf)
{
	drmModeRes *resources;
	drmModeConnector *connector = NULL;
	drmModeModeInfo * mode = NULL;
	struct myy_drm_caps const requested_caps[] = {
		{
			"DRM_CLIENT_CAP_UNIVERSAL_PLANES",
			DRM_CLIENT_CAP_UNIVERSAL_PLANES,
			1
		},
		{
			"DRM_CLIENT_CAP_ATOMIC",
			DRM_CLIENT_CAP_ATOMIC,
			1
		},
		{
			(char const *) 0, 0, 0
		}
	};
	int drm_fd = -1;
	int ret = -1;
	uint32_t crtc_index = 0;
	uint32_t crtc_id = NO_CRTC_FOUND;
	uint32_t plane_id = 0;

	drm_fd = open(drm_device_file, O_RDWR);
	
	if (drm_fd < 0) {
		LOGF("Could not open drm device\n");
		goto no_drm_device;
	}

	LOGVF("Opened %s successfully\n", drm_device_file);

	ret = myy_drm_set_caps(drm_fd, requested_caps);
	if (ret == -1)
	{
		LOGF("The device doesn't have the right capabilities");
		goto required_caps_not_available;
	}

	resources = drmModeGetResources(drm_fd);
	if (!resources) {
		printf("drmModeGetResources failed: %s\n", strerror(errno));
		goto no_drm_resources;
	}

	connector = drm_get_connector(drm_fd, resources);
	if (connector == NULL) {
		LOGF("No DRM connector...");
		goto no_drm_connector;
	}

	mode = drm_connect_select_best_resolution(connector);
	if (mode == NULL) {
		LOGF("No available resolutions...");
		goto no_drm_modes_useable;
	}

	crtc_id = drm_get_best_crtc(
		drm_fd, resources, connector, &crtc_index);
	if (crtc_id == NO_CRTC_FOUND) {
		LOGF("No CRTC useable with the selected connector...");
		goto no_drm_crtc_available;
	}

	plane_id = drm_get_primary_plane_for_crtc(drm_fd, crtc_index);
	if (plane_id == NO_PLANE_FOUND) {
		LOGF("No primary plane found !?");
		goto no_drm_primary_plane;
	}

	drmModeFreeResources(resources);

	myy_drm_conf->fd           = drm_fd;
	myy_drm_conf->mode         = *mode;
	myy_drm_conf->crtc_id      = crtc_id;
	myy_drm_conf->plane_id     = plane_id;
	myy_drm_conf->connector_id = connector->connector_id;
	myy_drm_conf->width        = mode->hdisplay;
	myy_drm_conf->height       = mode->vdisplay;

	return 0;

no_drm_primary_plane:
no_drm_crtc_available:
no_drm_modes_useable:
	drmModeFreeConnector(connector);
no_drm_connector:
	drmModeFreeResources(resources);
no_drm_resources:
required_caps_not_available:
	close(drm_fd);
no_drm_device:
	return -1;
}

/* drm_create_mode_handle ? */
static uint32_t drm_create_mode_id(
	myy_drm_infos_t * __restrict const myy_drm_conf)
{
	uint32_t mode_id = 0;

	int const ret = drmModeCreatePropertyBlob(
		myy_drm_conf->fd,
		&myy_drm_conf->mode,
		sizeof(myy_drm_conf->mode),
		&mode_id);
	if (ret != 0) {
		LOG_ERROR(
			"Could not create a 'property blob'\n"
			"Whatever that means...");
		mode_id = 0;
	}
	return mode_id;
}
static bool drm_map_framebuffer(
	myy_drm_infos_t * __restrict const myy_drm_conf)
{
	struct drm_mode_create_dumb dumb_create_req = { 0 };
	struct drm_mode_map_dumb dumb_map_req = { 0 };

	/* We won't use it. Go figure.
	 * It's a CPU mapped buffer.
	 * Why would would we use it with a GPU ?
	 * Got zero idea.
	 * But without it, nothing works.
	 */
	uint8_t * __restrict framebuffer;

	uint32_t fb = 0;
	int ret;
	int const drm_fd = myy_drm_conf->fd;
	uint32_t const width = myy_drm_conf->width;
	uint32_t const height = myy_drm_conf->height;

	dumb_create_req.width  = myy_drm_conf->width;
	dumb_create_req.height = myy_drm_conf->height;
	dumb_create_req.bpp    = 32;

	ret = drmIoctl(drm_fd,
		DRM_IOCTL_MODE_CREATE_DUMB,
		&dumb_create_req);
	if (ret < 0) {
		LOGF("Could create a dumb frame buffer.");
		goto create_dumb_buffer_failed;
	}

	ret = drmModeAddFB(
		drm_fd, width, height, 24, 32,
		dumb_create_req.pitch, dumb_create_req.handle,
		&fb);
	if (ret < 0) {
		LOG_ERROR("No framebuffer ?");
		goto no_frame_buffer;
	}

	dumb_map_req.handle = dumb_create_req.handle;

	ret = drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB,
		&dumb_map_req);
	if (ret) {
		LOG_ERROR("Unable to map dumb buffer.\n");
		goto could_not_map_dumb_buffer;
	}

	framebuffer = mmap(
		0, dumb_create_req.size, PROT_READ | PROT_WRITE,
		MAP_SHARED, drm_fd, dumb_map_req.offset);
	if (framebuffer == MAP_FAILED) {
		LOG_ERROR("Failed to mmap our framebuffer : %m\n");
		goto could_not_mmap_frame_buffer;
	}

	memset(framebuffer, 0, dumb_create_req.size);

	myy_drm_conf->framebuffer_id = fb;
	myy_drm_conf->framebuffer    = framebuffer;
	return true;

/* TODO Unmap framebuffer ? */
could_not_mmap_frame_buffer:
/* TODO Unmap dumb buffer ! */
could_not_map_dumb_buffer:
/* TODO Remove FB */
no_frame_buffer:
/* TODO Destroy dumb buffer ! */
create_dumb_buffer_failed:
	return false;
	
}

struct myy_kms_prop_id {
	char const * __restrict const name;
	uint32_t * __restrict const id;
};

static bool myy_drm_kms_get_prop_ids(
	int const drm_fd,
	uint32_t const object_id,
	uint32_t const object_type,
	struct myy_kms_prop_id const * __restrict const myy_props,
	size_t const n_props)
{
	drmModeObjectProperties * __restrict const drm_mode_props =
		drmModeObjectGetProperties(drm_fd, object_id, object_type);
	bool all_props_found = true;

	if (drm_mode_props == NULL) {
		LOG_ERROR(
			"drmModeObjectGetProperties returned NULL for I: %d, T: %d",
			 object_id, object_type);
		goto no_props;
	}

	for (uint32_t i = 0; i < drm_mode_props->count_props; i++) {
		drmModePropertyRes * __restrict const prop =
			drmModeGetProperty(drm_fd, drm_mode_props->props[i]);
		if (prop == NULL) {
			LOG_ERROR("The DRM driver is listing NULL properties...");
			goto invalid_prop;
		}

		for (uint32_t m = 0; m < n_props; m++) {
			struct myy_kms_prop_id looked_up_prop = myy_props[m];
			/* The longest property names we have are "MODE_ID" and
			 * "CRTC_ID". So that's 8 chars, '\0' accounted. */
			if (strncmp(looked_up_prop.name, prop->name, 8) == 0)
			{
				
				*looked_up_prop.id = prop->prop_id;
				LOGF("Property ID %s = %d\n",
					 looked_up_prop.name,
					 prop->prop_id);
				break;
			}
		}

		drmModeFreeProperty(prop);
	}

	/* NOTE: This assumes that properties were initialized to 0
	 * before calling this function.
	 */
	for (uint32_t m = 0; m < n_props; m++) {
		uint32_t const id_val = *(myy_props[m].id);
		bool const prop_found = ( id_val != 0 );
		if (!prop_found) {
			LOG_ERROR("Property %s was not found", myy_props[m].name);
		}
		all_props_found &= prop_found;
	}

	if (!all_props_found)
		goto not_all_props;

	return true;

invalid_prop:
	drmModeFreeObjectProperties(drm_mode_props);
not_all_props:
no_props:
	return false;
}

struct myy_drm_atomic_props_ids {
	struct {
		uint32_t mode_id;
		uint32_t active;
	} crtc;
	struct {
		uint32_t crtc_id;
	} connector;
	struct {
		uint32_t src_x;
		uint32_t src_y;
		uint32_t src_w;
		uint32_t src_h;
		uint32_t crtc_x;
		uint32_t crtc_y;
		uint32_t crtc_w;
		uint32_t crtc_h;
		uint32_t fb_id;
		uint32_t crtc_id;
	} plane;
};

static void myy_drm_atomic_props_ids_dump(
	struct myy_drm_atomic_props_ids * __restrict const ids)
{
	LOGF(
		"[myy_drm_atomic_props_ids]\n"
		"\tcrtc.mode_id      = %d\n"
		"\tcrtc.active       = %d\n"
		"\tconnector.crtc_id = %d\n"
		"\tplane.src_x       = %d\n"
		"\tplane.src_y       = %d\n"
		"\tplane.src_w       = %d\n"
		"\tplane.src_h       = %d\n"
		"\tplane.crtc_x      = %d\n"
		"\tplane.crtc_y      = %d\n"
		"\tplane.crtc_w      = %d\n"
		"\tplane.crtc_h      = %d\n"
		"\tplane.fb_id       = %d\n"
		"\tplane.crtc_id     = %d\n",
		ids->crtc.mode_id     ,
		ids->crtc.active      ,
		ids->connector.crtc_id,
		ids->plane.src_x      ,
		ids->plane.src_y      ,
		ids->plane.src_w      ,
		ids->plane.src_h      ,
		ids->plane.crtc_x     ,
		ids->plane.crtc_y     ,
		ids->plane.crtc_w     ,
		ids->plane.crtc_h     ,
		ids->plane.fb_id      ,
		ids->plane.crtc_id);
}

static bool myy_drm_atomic_get_props_ids(
	int const drm_fd,
	myy_drm_infos_t const * __restrict const myy_drm_conf,
	struct myy_drm_atomic_props_ids * __restrict const prop_ids)
{
	struct myy_kms_prop_id const crtc_props[] = {
		{ "MODE_ID", &prop_ids->crtc.mode_id      },
		{ "ACTIVE",  &prop_ids->crtc.active       },
	};

	struct myy_kms_prop_id const connector_props[] = {
		{ "CRTC_ID", &prop_ids->connector.crtc_id },
	};

	struct myy_kms_prop_id const plane_props[] = {
		{ "SRC_X",   &prop_ids->plane.src_x       },
		{ "SRC_Y",   &prop_ids->plane.src_y       },
		{ "SRC_W",   &prop_ids->plane.src_w       },
		{ "SRC_H",   &prop_ids->plane.src_h       },
		{ "CRTC_X",  &prop_ids->plane.crtc_x      },
		{ "CRTC_Y",  &prop_ids->plane.crtc_y      },
		{ "CRTC_W",  &prop_ids->plane.crtc_w      },
		{ "CRTC_H",  &prop_ids->plane.crtc_h      },
		{ "FB_ID",   &prop_ids->plane.fb_id       },
		{ "CRTC_ID", &prop_ids->plane.crtc_id     },
	};

	return (
		myy_drm_kms_get_prop_ids(
			drm_fd, myy_drm_conf->crtc_id,
			DRM_MODE_OBJECT_CRTC,
			crtc_props, ARRAY_SIZE(crtc_props))
		&& myy_drm_kms_get_prop_ids(
			drm_fd, myy_drm_conf->connector_id,
			DRM_MODE_OBJECT_CONNECTOR,
			connector_props, ARRAY_SIZE(connector_props))
		&& myy_drm_kms_get_prop_ids(
			drm_fd, myy_drm_conf->plane_id,
			DRM_MODE_OBJECT_PLANE,
			plane_props, ARRAY_SIZE(plane_props))
	);
}

#define myy_set_atomic_add_prop(request, element_id, prop_id, prop_val) \
	{\
		LOGF("drmModeAtomicAddProperty(\n"\
			"\t" #request    " : %p,\n"\
			"\t" #element_id " : %u,\n"\
			"\t" #prop_id    " : %u,\n"\
			"\t" #prop_val   " : %u);\n",\
			request, element_id, prop_id, prop_val); \
		int const ret_val = drmModeAtomicAddProperty(\
			request, element_id, prop_id, prop_val); \
		LOGF("-> %d", ret_val);\
	}


static bool drm_setup_atomic_mode_for_streams(
	myy_drm_infos_t const * __restrict const myy_drm_conf,
	uint32_t const mode_blob_id)
{
	/* Cache all the props. They will all be used, beside the
	 * CPU framebuffer address that we allocated for... no reason ? */
	myy_drm_infos_t const drm_conf = *myy_drm_conf;
	
	int const drm_fd              = drm_conf.fd;
	bool ret;
	int i_ret;

	drmModeAtomicReq * __restrict const atomic_request =
		drmModeAtomicAlloc();
	struct myy_drm_atomic_props_ids props_ids = { 0 };

	if (atomic_request == NULL) {
		LOG_ERROR("NO ATOMIC REQUEST ! OH NO !");
		goto no_atomic_request;
	}

	ret = myy_drm_atomic_get_props_ids(
		drm_fd, myy_drm_conf, &props_ids);
	if (ret == false) {
		LOG_ERROR("Some required DRM properties were not found :C");
		goto some_props_not_found;
	}

	/* TODO Some checks should be performed here */
	/* Copying NVIDIA comments */
	/* Myy : CRTC props */
	{
		/*
		 * Specify the mode to use on the CRTC,
		 * and make the CRTC active.
		 */
		myy_set_atomic_add_prop(
			atomic_request, drm_conf.crtc_id,
			props_ids.crtc.mode_id, mode_blob_id);
		myy_set_atomic_add_prop(
			atomic_request, drm_conf.crtc_id,
			props_ids.crtc.active, 1);
	}


	/* Myy : Connector props */
	{
		/* Tell the connector to receive pixels from the CRTC. */
		myy_set_atomic_add_prop(
			atomic_request,
			drm_conf.connector_id,
			props_ids.connector.crtc_id,
			drm_conf.crtc_id);
	}

	/* Myy : Plane props */
	{
		/* 
		 * Specify the region of source surface to display (i.e., the
		 * "ViewPortIn").  Note these values are in 16.16 format, so
		 * shift up by 16.
		 */

		myy_set_atomic_add_prop(
			atomic_request, drm_conf.plane_id,
			props_ids.plane.src_x, 0);
		myy_set_atomic_add_prop(
			atomic_request, drm_conf.plane_id,
			props_ids.plane.src_y, 0);
		myy_set_atomic_add_prop(
			atomic_request, drm_conf.plane_id,
			props_ids.plane.src_w, drm_conf.width << 16);
		myy_set_atomic_add_prop(
			atomic_request, drm_conf.plane_id,
			props_ids.plane.src_h, drm_conf.height << 16);

		/* 
		 * Specify the region within the mode where the image should be
		 * displayed (i.e., the "ViewPortOut").
		 */

		myy_set_atomic_add_prop(
			atomic_request, drm_conf.plane_id,
			props_ids.plane.crtc_x, 0);
		myy_set_atomic_add_prop(
			atomic_request, drm_conf.plane_id,
			props_ids.plane.crtc_y, 0);
		myy_set_atomic_add_prop(
			atomic_request, drm_conf.plane_id,
			props_ids.plane.crtc_w, drm_conf.width);
		myy_set_atomic_add_prop(
			atomic_request, drm_conf.plane_id,
			props_ids.plane.crtc_h, drm_conf.height);

		/*
		 * Specify the surface to display in the plane, and connect the
		 * plane to the CRTC.
		 *
		 * XXX for EGLStreams purposes, it would be nice to have the
		 * option of not specifying a surface at this point, as well as
		 * to be able to have the KMS atomic modeset consume a frame
		 * from an EGLStream.
		 */

		myy_set_atomic_add_prop(
			atomic_request, drm_conf.plane_id,
			props_ids.plane.fb_id, drm_conf.framebuffer_id);
		myy_set_atomic_add_prop(
			atomic_request, drm_conf.plane_id,
			props_ids.plane.crtc_id, drm_conf.crtc_id);
	}

	i_ret = drmModeAtomicCommit(
		drm_fd, atomic_request,
		DRM_MODE_ATOMIC_ALLOW_MODESET,
		NULL);
	if (i_ret != 0) {
		LOGF("Oh, the NVIDIA driver failed for no fucking reason ! %d\n",
			i_ret);
		goto could_not_commit;
	}

	drmModeAtomicFree(atomic_request);
	return true;

could_not_commit:
some_props_not_found:
	drmModeAtomicFree(atomic_request);
no_atomic_request:
	return false;
}
	

static int nvidia_attach_streams_to_drm(
	myy_drm_infos_t * __restrict const myy_drm_conf)
{
	uint32_t const mode_blob_id =
		drm_create_mode_id(myy_drm_conf);
	if (mode_blob_id == 0) {
		goto no_mode_blob_id;
	}

	if (!drm_map_framebuffer(myy_drm_conf)) {
		LOG_ERROR("Could not map frame_buffer");
		goto could_not_map_framebuffer;
	}

	if (!drm_setup_atomic_mode_for_streams(myy_drm_conf, mode_blob_id))
	{
		LOG_ERROR(
			"Could not setup DRM Atomic mode for NVIDIA EGLStreams");
		goto could_not_setup_atomic_mode_for_streams;
	}

	return 0;

could_not_setup_atomic_mode_for_streams:
/* TODO Unmap framebuffer */
could_not_map_framebuffer:
no_mode_blob_id:
	return -1;
}
static int nvidia_drm_open(
	struct myy_nvidia_functions const * __restrict const myy_nvidia,
	EGLDeviceEXT egl_device,
	myy_drm_infos_t * __restrict const myy_drm_conf)
{
	char const * __restrict const drm_device_filepath =
		myy_nvidia->eglQueryDeviceString(
			egl_device, EGL_DRM_DEVICE_FILE_EXT);

	LOGF("[NVIDIA] drm_device_filepath : %s\n",
		drm_device_filepath);
	int ret = 0;

	/* TODO This check should be performed while checking
	 * for devices...
	 */
	if (drm_device_filepath == NULL) {
		LOGF("We tried to use a device which doesn't seem to have "
		"an actual DRM device filepath (e.g. : /dev/dri/card0)\n");
		ret = -1;
		goto no_drm_device_filepath;
	}

	ret = drm_init(drm_device_filepath, myy_drm_conf);
	if (ret == -1) {
		LOG_ERROR(
			"Could not initialize the whole drm subsystem");
		goto could_not_initialise_drm;
	}

	ret = nvidia_attach_streams_to_drm(myy_drm_conf);
	if (ret == -1) {
		LOG_ERROR(
			"Could not connect NVIDIA EGL Streams to the DRM "
			"subsystem");
		goto could_not_attach_streams_to_kms;
	}

	return ret;

could_not_attach_streams_to_kms:
/* TODO drm_deinit */
could_not_initialise_drm:
no_drm_device_filepath:
	return -1;
}



static EGLBoolean egl_nvidia_get_config(
	EGLDisplay const egl_display,
	EGLConfig * __restrict const egl_config)
{
	/* The desired minimal configuration */
	static const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_STREAM_BIT_KHR, // Important one
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, // We want GLES 2.x
		EGL_RED_SIZE, 1,   // With RGB output
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 1, // With alpha
		EGL_DEPTH_SIZE, 1, // And a depth buffer
		EGL_NONE
	};
	/* Will be malloced */
	EGLConfig the_chosen_one;
	/* Will receive the number of available configurations */
	EGLint n_configs = 0;
	EGLBoolean ret = EGL_FALSE;

	ret = eglChooseConfig(
		egl_display, config_attribs, &the_chosen_one, 1, &n_configs);
	if (!ret || n_configs == 0) {
		LOG_EGL_ERROR(
			"Could not find a configuration with at least :\n"
			"- EGL Streams support\n"
			"- OpenGL ES 2.x support\n"
			"- RGB support\n"
			"- Alpha and Depth buffers support\n"
			"Call the police");
	}
	else {
		egl_print_config_attribs(egl_display, the_chosen_one);
		*egl_config = the_chosen_one;
	}

	return ret;
}

static EGLDisplay egl_nvidia_get_display(
	struct myy_nvidia_functions const * __restrict const nvidia,
	EGLDeviceEXT const nvidia_device,
	int const drm_fd)
{
	/*
	 * Provide the DRM fd when creating the EGLDisplay, so that the
	 * EGL implementation can make any necessary DRM calls using the
	 * same fd as the application.
	 */
	EGLint attribs[] = {
		EGL_DRM_MASTER_FD_EXT,
		drm_fd,
		EGL_NONE
	};

	return nvidia->eglGetPlatformDisplay(
		EGL_PLATFORM_DEVICE_EXT,
		(void*) nvidia_device, attribs);
}

static EGLBoolean nvidia_egl_create_surface(
	struct myy_nvidia_functions const * __restrict const nvidia,
	EGLDisplay egl_display,
	EGLConfig egl_config,
	myy_drm_infos_t const * __restrict const myy_drm_conf,
	EGLSurface * __restrict const egl_surface)
{
	EGLAttrib const layer_attribs[] = {
		EGL_DRM_PLANE_EXT,
		myy_drm_conf->plane_id,
		EGL_NONE,
	};

	EGLint const surface_attribs[] = {
		EGL_WIDTH, myy_drm_conf->width,
		EGL_HEIGHT, myy_drm_conf->height,
		EGL_NONE
	};

	EGLint const stream_attribs[] = { EGL_NONE };

	EGLOutputLayerEXT egl_layer;
	EGLStreamKHR egl_stream;
	EGLBoolean ret = EGL_FALSE;
	EGLSurface surface = EGL_NO_SURFACE;
	EGLint n;
	 /* Find the EGLOutputLayer that corresponds to the DRM KMS plane. */
	ret = nvidia->eglGetOutputLayers(
		egl_display, layer_attribs, &egl_layer, 1, &n);

    if (!ret || n == 0)
	{
		LOG_EGL_ERROR(
			"Unable to get EGLOutputLayer for plane 0x%08x\n",
			myy_drm_conf->plane_id);
		goto no_egl_output_layers;
	}

	/* Create an EGLStream. */
	egl_stream = nvidia->eglCreateStream(
		egl_display, stream_attribs);

	if (egl_stream == EGL_NO_STREAM_KHR) {
		LOG_EGL_ERROR("Unable to create stream.\n");
		goto no_egl_stream;
	}

	/* Set the EGLOutputLayer as the consumer of the EGLStream. */
	ret = nvidia->eglStreamConsumerOutput(
		egl_display,
		egl_stream,
		egl_layer);

	if (!ret) {
		LOG_EGL_ERROR("Unable to create EGLOutput stream consumer.\n");
		goto no_egl_stream_consumer_output;
	}

	/*
	 * EGL_KHR_stream defines that normally stream consumers need to
	 * explicitly retrieve frames from the stream.  That may be useful
	 * when we attempt to better integrate
	 * EGL_EXT_stream_consumer_egloutput with DRM atomic KMS requests.
	 * But, EGL_EXT_stream_consumer_egloutput defines that by default:
	 *
	 *   On success, <layer> is bound to <stream>, <stream> is placed
	 *   in the EGL_STREAM_STATE_CONNECTING_KHR state, and EGL_TRUE is
	 *   returned.  Initially, no changes occur to the image displayed
	 *   on <layer>. When the <stream> enters state
	 *   EGL_STREAM_STATE_NEW_FRAME_AVAILABLE_KHR, <layer> will begin
	 *   displaying frames, without further action required on the
	 *   application's part, as they become available, taking into
	 *   account any timestamps, swap intervals, or other limitations
	 *   imposed by the stream or producer attributes.
	 *
	 * So, eglSwapBuffers() (to produce new frames) is sufficient for
	 * the frames to be displayed.  That behavior can be altered with
	 * the EGL_EXT_stream_acquire_mode extension.
	 */

	/*
	 * Create an EGLSurface as the producer of the EGLStream.  Once
	 * the stream's producer and consumer are defined, the stream is
	 * ready to use.  eglSwapBuffers() calls for the EGLSurface will
	 * deliver to the stream's consumer, i.e., the DRM KMS plane
	 * corresponding to the EGLOutputLayer.
	 */

	surface = nvidia->eglCreateStreamProducerSurface(
		egl_display, egl_config, egl_stream, surface_attribs);

	if (surface == EGL_NO_SURFACE) {
		LOG_EGL_ERROR(
			"Could not create a surface through NVIDIA means\n");
		goto no_egl_surface;
	}

	*egl_surface = surface;

	return EGL_TRUE;

no_egl_surface:
/* eglDestroyStreamProducerSurface ? */
no_egl_stream_consumer_output:
/* Destroy Stream here */

no_egl_stream:
no_egl_output_layers:
	return EGL_FALSE;
}

static int egl_prepare_opengl_context(
	struct myy_nvidia_functions const * __restrict const nvidia,
	EGLDeviceEXT const nvidia_device,
	myy_drm_infos_t * __restrict const myy_drm_conf,
	myy_opengl_infos_t * __restrict const myy_gl_conf)
{
	EGLint major, minor;
	EGLBoolean egl_ret = EGL_FALSE;
	EGLDisplay display;
	EGLConfig config;
	EGLContext context;
	EGLSurface surface;

	EGLint const context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	display = egl_nvidia_get_display(
		nvidia, nvidia_device, myy_drm_conf->fd);
	if (display == EGL_NO_DISPLAY) {
		LOG_EGL_ERROR("No display John !");
		goto no_egl_display;
	}

	if (!eglInitialize(display, &major, &minor)) {
		LOG_EGL_ERROR("Could not initialize the display");
		goto cannot_initialize_egl;
	}

	LOGF("Using display %p with EGL version %d.%d",
		display, major, minor);

	LOGF("EGL Version \"%s\"", eglQueryString(display, EGL_VERSION));
	LOGF("EGL Vendor \"%s\"", eglQueryString(display, EGL_VENDOR));
	LOGF("EGL Extensions \"%s\"", eglQueryString(display, EGL_EXTENSIONS));

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		LOG_EGL_ERROR(
			"Failed to bind api EGL_OPENGL_ES_API");
		goto no_opengl_es_api;
	}

	egl_ret = egl_nvidia_get_config(display, &config);
	if (egl_ret == EGL_FALSE) {
		LOGF("No config :C");
		goto no_egl_config;
	}

	context = eglCreateContext(display, config,
		EGL_NO_CONTEXT, context_attribs);
	if (context == NULL) {
		LOG_EGL_ERROR(
			"Failed to create an OpenGL ES 2.x context\n");
		goto no_egl_context;
	}

	egl_ret = nvidia_egl_create_surface(
		nvidia, display, config, myy_drm_conf, &surface);

	if (!egl_ret) {
		LOG_ERROR("No surface !?");
		goto no_egl_surface;
	}

	egl_ret = eglMakeCurrent(
		display, surface, surface, context);

	if (!egl_ret) {
		LOG_ERROR(
			"Could not the surface current... ???");
		goto no_egl_make_current_failed;
	}

	myy_gl_conf->display = display;
	myy_gl_conf->config  = config;
	myy_gl_conf->context = context;
	myy_gl_conf->surface = surface;
	return 0;

no_egl_make_current_failed:
/* TODO Destroy surface here */
no_egl_surface:
/* TODO Destroy context here */
no_egl_context:
no_egl_config:
no_opengl_es_api:
cannot_initialize_egl:
no_egl_display:
	return -1;
}

/* Draw code here */
static void draw(uint32_t i)
{
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glClearColor(0.2f, 0.3f, 0.5f, 1.0f);
}


static void page_flip_handler(int fd, unsigned int frame,
		  unsigned int sec, unsigned int usec, void *data)
{
	int *waiting_for_flip = data;
	*waiting_for_flip = 0;
}

int egl_check_extensions_client(void)
{
	char const * __restrict const extension_names[] = {
		"EGL_EXT_device_base",
		"EGL_EXT_device_enumeration",
		"EGL_EXT_device_query",
		"EGL_EXT_platform_base",
		"EGL_EXT_platform_device",
		(char *) 0
	};

	char const * __restrict const client_extensions_list = 
		eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
	return egl_strstr(client_extensions_list, extension_names, "client");
}

int myy_nvidia_functions_prepare(
	struct myy_nvidia_functions * __restrict const myy_nvidia)
{
	/* Fragile code, but this makes the user aware of all the
	 * extensions he needs at once.
	 * Now, this array must be synchronized (manually) with
	 * the structure of function pointers "myy_nvidia_functions"
	 */
	char const * __restrict const extension_names[] = {
		"eglQueryDevicesEXT",
		"eglQueryDeviceStringEXT",
		"eglGetPlatformDisplayEXT",
		"eglGetOutputLayersEXT",
		"eglCreateStreamKHR",
		"eglStreamConsumerOutputEXT",
		"eglCreateStreamProducerSurfaceKHR",
		(char *) 0
	};
	char const * __restrict const * __restrict cursor = extension_names;

	void (**extensions_addresses)() = (void(**)()) myy_nvidia;
	int everything_is_ok = 0;

	while (*cursor != 0) {
		char const * __restrict const ext_name = *cursor;
		*extensions_addresses = eglGetProcAddress(ext_name);
		if (*extensions_addresses == NULL) {
			everything_is_ok = -1;
			fprintf(stderr, "Extension '%s' not found :C\n", ext_name);
			/* We'll still check for the other extensions
			 * anyway, so that the user knows about EVERY
			 * single extension he needs at once.
			 */
		}
		extensions_addresses++;
		cursor++;
	}

	return everything_is_ok;
}

int nvidia_egl_get_device(
	struct myy_nvidia_functions * __restrict const myy_nvidia,
	EGLDeviceEXT * __restrict const ret_device)
{
	int ret = 0;
	EGLint n_devices, i;
	EGLDeviceEXT *devices = NULL;
	EGLDeviceEXT device = EGL_NO_DEVICE_EXT;
	EGLBoolean egl_ret;
	int chosen_device = -1;

	/* Query how many devices are present. */
	egl_ret = myy_nvidia->eglQueryDevices(0, NULL, &n_devices);

	if (!egl_ret) {
		LOGF("Failed to query EGL devices.");
		goto out;
	}

	if (n_devices < 1) {
		LOGF("No EGL devices found.");
		goto out;
	}

	/* Allocate memory to store that many EGLDeviceEXTs. */
	devices = calloc(n_devices, sizeof(EGLDeviceEXT));

	if (devices == NULL) {
		LOGF("Memory allocation failure.");
		goto out;
	}

	/* Query the EGLDeviceEXTs. */
	egl_ret = myy_nvidia->eglQueryDevices(
		n_devices, devices, &n_devices);

	if (!egl_ret) {
		LOGF("Failed to query EGL devices.");
		goto could_not_query_devices;
	}

	char const * __restrict const checked_extensions[] = {
		"EGL_EXT_device_drm",
		(char *) 0
	};

	for (i = 0; i < n_devices; i++) {

		char const * __restrict const device_extensions =
			myy_nvidia->eglQueryDeviceString(
				devices[i], EGL_EXTENSIONS);
		LOGF("Device[%d/%d] - Extensions : \n%s",
			 i, n_devices, device_extensions);

		if (chosen_device < 0)
		{
			ret = egl_strstr(
				device_extensions, checked_extensions, "devices");
			if (ret == 0) {
				device = devices[i];
				chosen_device = i;
			}
			/* Continue, in order to list all the available
			 * devices, for demonstration purposes.
			 */
		}
	}

could_not_query_devices:
	free(devices);

	if (chosen_device >= 0) {
		LOGVF("Using device %d", chosen_device);
		ret = 0;
	}

out:
	*ret_device = device;
	if (device == EGL_NO_DEVICE_EXT) {
		fprintf(
			stderr,
			"No devices supporting the right EGL extensions "
			"were found.\n");
		ret = -1;
	}

	return ret;
}

int main(int argc, char *argv[])
{
	fd_set fds;
	drmEventContext evctx = {
		.version = DRM_EVENT_CONTEXT_VERSION,
		.page_flip_handler = page_flip_handler,
	};
	struct drm_fb *fb;
	uint32_t i = 0;
	int ret;
	struct myy_nvidia_functions myy_nvidia;
	myy_drm_infos_t drm;
	myy_opengl_infos_t gl;

	ret = myy_nvidia_functions_prepare(&myy_nvidia);
	if (ret) {
		LOG_ERROR(
			"Failed to get the EGL extensions functions addresses "
			"from your current driver.\n"
			"This example uses NVIDIA specific extensions so be "
			"sure to use NVIDIA OpenGL drivers.");
		return ret;
	}

	ret = egl_check_extensions_client();
	if (ret) {
		LOG_ERROR(
			"... You got the right drivers but not the right "
			"extensions on your EGL client...\n"
			"File a bug report with the output of this program "
			"to : \n"
			"https://github.com/Miouyouyou/nvidia-drm-kms");
		return ret;
	}

	/* TODO Move inside the struct */
	EGLDeviceEXT nvidia_device;
	ret = nvidia_egl_get_device(&myy_nvidia, &nvidia_device);
	if (ret) {
		LOG_ERROR(
			"Something went wrong while trying to prepare the "
			"device.\n"
			"File a bug report to : \n"
			"https://github.com/Miouyouyou/nvidia-drm-kms");
		return ret;
	}

	ret = nvidia_drm_open(&myy_nvidia, nvidia_device, &drm);
	if (ret) {
		LOG_ERROR(
			"Failed to initialize DRM through NVIDIA means");
		return ret;
	}
	myy_drm_config_dump(&drm);


	ret = egl_prepare_opengl_context(
		&myy_nvidia, nvidia_device, &drm, &gl);
	if (ret) {
		LOG_ERROR(
			"Failed to initialize EGL through NVIDIA means");
		return ret;
	}

	while (1) {
		draw(i++);
		if (!eglSwapBuffers(gl.display, gl.surface)) {
			LOG_ERROR(
				"Could not swap the buffers !? CALL THE POLICE !\n"
				"Error : %d", eglGetError());
		}
	}

	return ret;
}
