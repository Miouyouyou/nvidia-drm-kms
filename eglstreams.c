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

static struct {
	int fd;
	drmModeModeInfo *mode;
	uint32_t crtc_id;
	uint32_t connector_id;
} drm;

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


static uint32_t find_crtc_for_encoder(const drmModeRes *resources,
				      const drmModeEncoder *encoder) {
	int i;

	for (i = 0; i < resources->count_crtcs; i++) {
		/* possible_crtcs is a bitmask as described here:
		 * https://dvdhrm.wordpress.com/2012/09/13/linux-drm-mode-setting-api
		 */
		const uint32_t crtc_mask = 1 << i;
		const uint32_t crtc_id = resources->crtcs[i];
		if (encoder->possible_crtcs & crtc_mask) {
			return crtc_id;
		}
	}

	/* no match found */
	return -1;
}

static uint32_t find_crtc_for_connector(const drmModeRes *resources,
					const drmModeConnector *connector) {
	int i;

	for (i = 0; i < connector->count_encoders; i++) {
		const uint32_t encoder_id = connector->encoders[i];
		drmModeEncoder *encoder = drmModeGetEncoder(drm.fd, encoder_id);

		if (encoder) {
			const uint32_t crtc_id = find_crtc_for_encoder(resources, encoder);

			drmModeFreeEncoder(encoder);
			if (crtc_id != 0) {
				return crtc_id;
			}
		}
	}

	/* no match found */
	return -1;
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



static int init_drm(
	char const * __restrict const drm_device_file)
{
	drmModeRes *resources;
	drmModeConnector *connector = NULL;
	drmModeEncoder *encoder = NULL;
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

	drm.fd = open(drm_device_file, O_RDWR);
	
	if (drm.fd < 0) {
		LOGF("could not open drm device\n");
		return -1;
	}
	else {
		LOGVF("Opened %s successfully\n", drm_device_file);
	}

	if (myy_drm_set_caps(drm.fd, requested_caps) < 0)
	{
		LOGF("The device doesn't have the right capabilities");
		return -1;
	}

	resources = drmModeGetResources(drm.fd);
	if (!resources) {
		printf("drmModeGetResources failed: %s\n", strerror(errno));
		return -1;
	}

	/* find a connected connector: */
	for (i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(drm.fd, resources->connectors[i]);
		if (connector->connection == DRM_MODE_CONNECTED) {
			/* it's connected, let's use this! */
			break;
		}
		drmModeFreeConnector(connector);
		connector = NULL;
	}

	if (!connector) {
		/* we could be fancy and listen for hotplug events and wait for
		 * a connector..
		 */
		printf("no connected connector!\n");
		return -1;
	}

	/* find prefered mode or the highest resolution mode: */
	for (i = 0, area = 0; i < connector->count_modes; i++) {
		drmModeModeInfo *current_mode = &connector->modes[i];

		if (current_mode->type & DRM_MODE_TYPE_PREFERRED) {
			drm.mode = current_mode;
		}

		int current_area = current_mode->hdisplay * current_mode->vdisplay;
		if (current_area > area) {
			drm.mode = current_mode;
			area = current_area;
		}
	}

	if (!drm.mode) {
		printf("could not find mode!\n");
		return -1;
	}

	/* find encoder: */
	for (i = 0; i < resources->count_encoders; i++) {
		encoder = drmModeGetEncoder(drm.fd, resources->encoders[i]);
		if (encoder->encoder_id == connector->encoder_id)
			break;
		drmModeFreeEncoder(encoder);
		encoder = NULL;
	}

	if (encoder) {
		drm.crtc_id = encoder->crtc_id;
	} else {
		uint32_t crtc_id = find_crtc_for_connector(resources, connector);
		if (crtc_id == 0) {
			printf("no crtc found!\n");
			return -1;
		}

		drm.crtc_id = crtc_id;
	}

	drm.connector_id = connector->connector_id;

	return 0;
}

static int nvidia_drm_open(
	struct myy_nvidia_functions const * __restrict const myy_nvidia,
	EGLDeviceEXT egl_device)
{
	char const * __restrict const drm_device_filepath =
		myy_nvidia->eglQueryDeviceString(
			egl_device, EGL_DRM_DEVICE_FILE_EXT);

	int ret = 0;

	/* TODO This check should be performed while checking
	 * for devices...
	 */
	if (drm_device_filepath == NULL) {
		LOGF("We tried to use a device which doesn't seem to have "
		"an actual DRM device filepath (e.g. : /dev/dri/card0)\n");
		ret = -1;
	}

	ret = init_drm(drm_device_filepath);
	return ret;
}

/*static int init_gl(void)
{
	EGLint major, minor, n;
	GLuint vertex_shader, fragment_shader;
	GLint ret;

	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	static const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 0,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};

	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = NULL;
	get_platform_display =
		(void *) eglGetProcAddress("eglGetPlatformDisplayEXT");
	assert(get_platform_display != NULL);

	gl.display = get_platform_display(EGL_PLATFORM_DEVICE_EXT, gbm.dev, NULL);

	if (!eglInitialize(gl.display, &major, &minor)) {
		printf("failed to initialize\n");
		return -1;
	}

	printf("Using display %p with EGL version %d.%d\n",
			gl.display, major, minor);

	printf("EGL Version \"%s\"\n", eglQueryString(gl.display, EGL_VERSION));
	printf("EGL Vendor \"%s\"\n", eglQueryString(gl.display, EGL_VENDOR));
	printf("EGL Extensions \"%s\"\n", eglQueryString(gl.display, EGL_EXTENSIONS));

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		printf("failed to bind api EGL_OPENGL_ES_API\n");
		return -1;
	}

	if (!eglChooseConfig(gl.display, config_attribs, &gl.config, 1, &n) || n != 1) {
		printf("failed to choose config: %d\n", n);
		return -1;
	}

	gl.context = eglCreateContext(gl.display, gl.config,
			EGL_NO_CONTEXT, context_attribs);
	if (gl.context == NULL) {
		printf("failed to create context\n");
		return -1;
	}

	gl.surface = eglCreateWindowSurface(gl.display, gl.config, gbm.surface, NULL);
	if (gl.surface == EGL_NO_SURFACE) {
		printf("failed to create egl surface\n");
		return -1;
	}*/

	/* connect the context to the surface */
	/*eglMakeCurrent(gl.display, gl.surface, gl.surface, gl.context);

	printf("GL Extensions: \"%s\"\n", glGetString(GL_EXTENSIONS));

	return 0;
}*/

/* Draw code here */
static void draw(uint32_t i)
{
	glClear(GL_COLOR_BUFFER_BIT);
	glClearColor(0.2f, 0.3f, 0.5f, 1.0f);
}

/*static struct drm_fb * drm_fb_get_from_bo(struct gbm_bo *bo)
{
	struct drm_fb *fb = gbm_bo_get_user_data(bo);
	uint32_t width, height, stride, handle;
	int ret;

	if (fb)
		return fb;

	fb = calloc(1, sizeof *fb);
	fb->bo = bo;

	width = gbm_bo_get_width(bo);
	height = gbm_bo_get_height(bo);
	stride = gbm_bo_get_stride(bo);
	handle = gbm_bo_get_handle(bo).u32;

	ret = drmModeAddFB(drm.fd, width, height, 24, 32, stride, handle, &fb->fb_id);
	if (ret) {
		printf("failed to create fb: %s\n", strerror(errno));
		free(fb);
		return NULL;
	}

	gbm_bo_set_user_data(bo, fb, drm_fb_destroy_callback);

	return fb;
}*/

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

	/*
	* Select which EGLDeviceEXT to use.
	*
	* The EGL_EXT_device_query extension defines the functions:
	*
	*   eglQueryDeviceAttribEXT()
	*   eglQueryDeviceStringEXT()
	*
	* as ways to generically query properties of EGLDeviceEXTs, and
	* separate EGL extensions define EGLDeviceEXT attributes that can
	* be queried through those functions.  E.g.,
	*
	* - EGL_NV_device_cuda lets you query the CUDA device ID
	*   (EGL_CUDA_DEVICE_NV of an EGLDeviceEXT.
	*
	* - EGL_EXT_device_drm lets you query the DRM device file
	*   (EGL_DRM_DEVICE_FILE_EXT) of an EGLDeviceEXT.
	*
	* Future extensions could define other EGLDeviceEXT attributes
	* such as PCI BusID.
	*
	* For now, just choose the first device that supports EGL_EXT_device_drm.
	*/
// 
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

	ret = nvidia_drm_open(&myy_nvidia, nvidia_device);
	if (ret) {
		LOGF("failed to initialize DRM");
		return ret;
	}

	close(drm.fd);
	exit(1);
	
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
