#ifndef __YUAN_SERVER_NAS_H__
#define __YUAN_SERVER_NAS_H__

#include "nas/nas_auth_service.h"
#include "nas/nas_http_middleware.h"
#include "nas/nas_metadata_store.h"
#include "nas/nas_permission_service.h"
#include "nas/nas_redis_webdav_lock_manager.h"
#if YUAN_NAS_HAS_REDIS
#include "nas/nas_redis_metadata_store.h"
#endif
#include "nas/nas_share_manager.h"
#include "nas/nas_types.h"
#include "nas/nas_webdav_adapter.h"
#include "nas/nas_webdav_backend.h"
#include "nas/nas_webdav_mount.h"

#endif
