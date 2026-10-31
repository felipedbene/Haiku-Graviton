/* Minimal Node-API addon for the node-gyp acceptance check: add(a, b). */
#include <node_api.h>

static napi_value Add(napi_env env, napi_callback_info info)
{
	size_t argc = 2;
	napi_value argv[2], result;
	double a, b;
	if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok || argc < 2)
		return NULL;
	napi_get_value_double(env, argv[0], &a);
	napi_get_value_double(env, argv[1], &b);
	napi_create_double(env, a + b, &result);
	return result;
}

static napi_value Init(napi_env env, napi_value exports)
{
	napi_value fn;
	napi_create_function(env, "add", NAPI_AUTO_LENGTH, Add, NULL, &fn);
	napi_set_named_property(env, exports, "add", fn);
	return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
