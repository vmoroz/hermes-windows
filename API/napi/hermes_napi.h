#include "js_native_ext_api.h"

// Creates new Hermes napi_env with ref count 1.
NAPI_EXTERN napi_status __cdecl napi_create_hermes_env(napi_env *env);
