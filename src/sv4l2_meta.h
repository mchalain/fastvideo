#ifndef __SV4L2_META_H__
#define __SV4L2_META_H__

/// defined into framebuffer.h
typedef struct FrameBuffer_s FrameBuffer_t;

/// defined into unixsocket.h
typedef struct client_s client_t;

typedef struct V4l2_Meta_Conf_s V4l2_Meta_Conf_t;
struct V4l2_Meta_Conf_s
{
	DeviceConf_t parent;
	const char *server_path;
};

typedef struct V4l2_Meta_s V4l2_Meta_t;
struct V4l2_Meta_s
{
	const char *name;
	uint32_t fourcc;
	struct
	{
		/**
		 * @brief create a context to manage the buffer
		 * @param client a connection to the fastsetting application
		 * @param config the configuration of the plugin
		 * @return context
		 */
		void *(*create)(client_t *client, V4l2_Meta_Conf_t *config);
		/**
		 * @brief queue new meta data buffer
		 * @param ctx the context from "create"
		 * @param buffer the structure on the meta data
		 * @return 0 on success otherwise -1
		 */
		int (*queue)(void *ctx, FrameBuffer_t *buffer);
		/**
		 * @brief dequeue the last buffer treated
		 * @param ctx the context from "create"
		 * @param buffer the structure on the meta data
		 * @return 0 on success otherwise -1
		 */
		int (*dequeue)(void *ctx, FrameBuffer_t *buffer);
		/**
		 * @brief free the context
		 * @param ctx the context from "create"
		 */
		void (*destroy)(void *ctx);
	} ops;
};

extern FastVideoDevice_ops_t ssv4l2_meta_ops;

#endif
