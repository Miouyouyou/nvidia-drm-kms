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

#include <xf86drm.h>
#include <xf86drmMode.h>

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

static struct {
	EGLDisplay display;
	EGLConfig config;
	EGLContext context;
	EGLSurface surface;
	GLuint program;
	GLint modelviewmatrix, modelviewprojectionmatrix, normalmatrix;
	GLuint vbo;
	GLuint positionsoffset, colorsoffset, normalsoffset;
} gl;

struct myy_drm_infos {
	int fd;
	drmModeModeInfo * mode;
	uint32_t crtc_id;
	uint32_t plane_id;
	uint32_t connector_id;
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
	uint32_t const n_crtcs;
	for (uint32_t i = 0;
	     (selected_crtc == NO_CRTC_FOUND) & (i < n_crtcs);
	     i++)
	{
		/* possible_crtcs is a bitmask as described here:
		 * https://dvdhrm.wordpress.com/2012/09/13/linux-drm-mode-setting-api
		 */
		uint32_t const crtc_mask = 1 << i;
		if (encoder->possible_crtcs & crtc_mask) {
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
	for (int i = 0;
	     (crtc_id != NO_CRTC_FOUND) & (i < n_encoders);
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
		mode->name ? mode->name : "(null)");
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
			LOGF("Could not set property %s.\n", caps->name);
			ret = -1;
			/* Keep going, enumerate all issues and provide
			 * meaningful error messages.
			 * Then fail at the end.
			 */
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

	for (uint32_t i = 0; (found != 0) & (i < n_props); i++) {

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
	uint32_t n_planes = 0;
	drmModePlaneRes * __restrict const planes_resources =
		drmModeGetPlaneResources(drm_fd);

	if (planes_resources != NULL) {
		uint32_t const n_planes = planes_resources->count_planes;

		for (uint32_t i = 0;
			(plane_id != NO_PLANE_FOUND) & (i < n_planes);
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

static int init_drm(
	char const * __restrict const drm_device_file,
	myy_drm_infos_t * __restrict const myy_drm_conf)
{
	drmModeRes *resources;
	drmModeConnector *connector = NULL;
	drmModeModeInfo * mode = NULL;
	struct myy_drm_caps const requested_caps[] = {
		{
			"DRM_CLIENT_CAP_UNIVERSAL_PLANES", DRM_CLIENT_CAP_UNIVERSAL_PLANES,
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
	int i, area;
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

	myy_drm_conf->fd           = drm_fd;
	myy_drm_conf->mode         = mode;
	myy_drm_conf->crtc_id      = crtc_id;
	myy_drm_conf->plane_id     = plane_id;
	myy_drm_conf->connector_id = connector->connector_id;

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
	}

	ret = init_drm(drm_device_filepath, myy_drm_conf);
	return ret;
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
		LOG_ERROR(
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

static int egl_prepare_opengl_context(
	struct myy_nvidia_functions const * __restrict const nvidia,
	EGLDeviceEXT const nvidia_device,
	int const drm_fd)
{
	EGLint major, minor, n;
	GLuint vertex_shader, fragment_shader;
	GLint ret;
	EGLBoolean egl_ret = EGL_FALSE;
	EGLDisplay display;
	EGLConfig config;
	EGLContext context;

	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	display = egl_nvidia_get_display(
		nvidia, nvidia_device, drm_fd);
	if (display == EGL_NO_DISPLAY) {
		LOG_ERROR("No display John !");
		return -1;
	}

	if (!eglInitialize(gl.display, &major, &minor)) {
		LOG_ERROR("Could not initialize the display");
		return -1;
	}

	LOGF("Using display %p with EGL version %d.%d",
		gl.display, major, minor);

	LOGF("EGL Version \"%s\"", eglQueryString(gl.display, EGL_VERSION));
	LOGF("EGL Vendor \"%s\"", eglQueryString(gl.display, EGL_VENDOR));
	LOGF("EGL Extensions \"%s\"", eglQueryString(gl.display, EGL_EXTENSIONS));

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		LOG_ERROR("failed to bind api EGL_OPENGL_ES_API");
		return -1;
	}

	egl_ret = egl_nvidia_get_config(display, &config);
	if (egl_ret) {
		LOGF("No config :C");
		return -1;
	}

	context = eglCreateContext(gl.display, gl.config,
			EGL_NO_CONTEXT, context_attribs);
	if (gl.context == NULL) {
		printf("failed to create context\n");
		return -1;
	}

	/*nvidia->eglGetOutputLayers
	nvidia->eglCreateStream
	nvidia->eglStreamConsumerOutput*/

	/*gl.surface = eglCreateWindowSurface(gl.display, gl.config, gbm.surface, NULL);
	if (gl.surface == EGL_NO_SURFACE) {
		printf("failed to create egl surface\n");
		return -1;
	}*/

	/*gl.config  = config;
	gl.display = display;*/
	/* connect the context to the surface */
	/*eglMakeCurrent(gl.display, gl.surface, gl.surface, gl.context);

	printf("GL Extensions: \"%s\"\n", glGetString(GL_EXTENSIONS));*/
	
	return 0;
}

/* Draw code here */
static void draw(uint32_t i)
{
	glClear(GL_COLOR_BUFFER_BIT);
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

int egl_check_extensions_nvidia_device(
	struct myy_nvidia_functions * __restrict const myy_nvidia)
{
}

int egl_check_extensions_display(EGLDisplay egl_display)
{
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

	ret = myy_nvidia_functions_prepare(&myy_nvidia);
	if (ret) {
		LOGF(
			"Failed to get the EGL extensions functions addresses "
			"from your current driver.\n"
			"This example uses NVIDIA specific extensions so be "
			"sure to use NVIDIA OpenGL drivers.");
		return ret;
	}

	ret = egl_check_extensions_client();
	if (ret) {
		LOGF(
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
		LOGF(
			"Something went wrong while trying to prepare the "
			"device.\n"
			"File a bug report to : \n"
			"https://github.com/Miouyouyou/nvidia-drm-kms");
		return ret;
	}

	ret = nvidia_drm_open(&myy_nvidia, nvidia_device, &drm);
	if (ret) {
		LOGF("failed to initialize DRM");
		return ret;
	}
	close(drm.fd);
	exit(1);


	ret = egl_prepare_opengl_context(
		&myy_nvidia, nvidia_device, drm.fd);
	if (ret) {
		printf("failed to initialize EGL\n");
		return ret;
	}

	/* clear the color buffer */
	glClearColor(0.5, 0.5, 0.5, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);
	eglSwapBuffers(gl.display, gl.surface);

	/* set mode: */
	ret = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0,
			&drm.connector_id, 1, drm.mode);
	if (ret) {
		printf("failed to set mode: %s\n", strerror(errno));
		return ret;
	}

	while (1) {
		int waiting_for_flip = 1;

		draw(i++);

		eglSwapBuffers(gl.display, gl.surface);

		/*
		 * Here you could also update drm plane layers if you want
		 * hw composition
		 */

		ret = drmModePageFlip(drm.fd, drm.crtc_id, fb->fb_id,
				DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip);
		if (ret) {
			printf("failed to queue page flip: %s\n", strerror(errno));
			return -1;
		}

		while (waiting_for_flip) {
			ret = select(drm.fd + 1, &fds, NULL, NULL, NULL);
			if (ret < 0) {
				printf("select err: %s\n", strerror(errno));
				return ret;
			} else if (ret == 0) {
				printf("select timeout!\n");
				return -1;
			} else if (FD_ISSET(0, &fds)) {
				printf("user interrupted!\n");
				break;
			}
			drmHandleEvent(drm.fd, &evctx);
		}

		/* release last buffer to render on again: */
	}

	return ret;
}
