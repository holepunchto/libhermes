#include <atomic>
#include <cmath>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <assert.h>
#include <ctype.h>
#include <intrusive.h>
#include <intrusive/list.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utf.h>
#include <uv.h>

#include <hermes/hermes.h>
#include <jsi/instrumentation.h>
#include <jsi/jsi.h>

#include <js.h>

using namespace facebook;

typedef struct js_callback_s js_callback_t;
typedef struct js_finalizer_s js_finalizer_t;
typedef struct js_native_state_s js_native_state_t;
typedef struct js_teardown_task_s js_teardown_task_t;
typedef struct js_teardown_queue_s js_teardown_queue_t;
typedef struct js_threadsafe_queue_s js_threadsafe_queue_t;

struct js_deferred_teardown_s {
  js_env_t *env;
};

struct js_teardown_task_s {
  enum {
    js_immediate_teardown,
    js_deferred_teardown,
  } type;

  union {
    struct {
      js_teardown_cb cb;
    } immediate;

    struct {
      js_deferred_teardown_t handle;
      js_deferred_teardown_cb cb;
    } deferred;
  };

  void *data;
  intrusive_list_node_t list;
};

struct js_teardown_queue_s {
  intrusive_list_t tasks;
};

struct js_platform_s {
  js_platform_options_t options;
  uv_loop_t *loop;
};

struct js_env_s {
  uv_loop_t *loop;
  uv_async_t teardown;
  int active_handles;

  js_platform_t *platform;
  js_handle_scope_t *scope;
  js_handle_scope_t *root_scope;

  uint32_t refs;
  uint32_t depth;

  std::unique_ptr<facebook::hermes::HermesRuntime> runtime;

  std::optional<jsi::Value> exception;
  std::optional<jsi::Value> bindings;
  std::optional<jsi::Value> default_module_id;
  std::optional<jsi::Value> function_id_key;

  int64_t external_memory;

  std::atomic<bool> terminated;
  bool destroying;

  js_teardown_queue_t teardown_queue;

  // Everything that holds a value on behalf of the caller beyond the lifetime
  // of a handle scope. The values must be released before the runtime is torn
  // down, as the runtime asserts that no values outlive it.
  intrusive_list_t references;
  intrusive_list_t deferreds;
  intrusive_list_t scripts;
  intrusive_list_t backing_stores;
  intrusive_list_t threadsafe_functions;

  struct {
    js_uncaught_exception_cb uncaught_exception;
    void *uncaught_exception_data;

    js_unhandled_rejection_cb unhandled_rejection;
    void *unhandled_rejection_data;
  } callbacks;

  jsi::Runtime &
  rt() {
    return *runtime;
  }
};

struct js_handle_scope_s {
  js_handle_scope_t *parent;
  std::deque<jsi::Value> values;
};

struct js_escapable_handle_scope_s : js_handle_scope_s {};

struct js_script_s {
  std::shared_ptr<const jsi::PreparedJavaScript> prepared;
  std::optional<jsi::Value> id;
  char *name;
  intrusive_list_node_t list;
};

struct js_ref_s {
  std::optional<jsi::Value> value;
  std::optional<jsi::WeakObject> weak;
  uint32_t count;
  intrusive_list_node_t list;
};

struct js_deferred_s {
  std::optional<jsi::Value> resolve;
  std::optional<jsi::Value> reject;
  intrusive_list_node_t list;
};

struct js_string_view_s {
  js_string_encoding_t encoding;
  std::string latin1;
  std::u16string utf16;
};

struct js_finalizer_s {
  js_env_t *env;
  void *data;
  js_finalize_cb finalize_cb;
  void *finalize_hint;

  void
  finalize() {
    if (finalize_cb) finalize_cb(env, data, finalize_hint);
  }
};

struct js_native_state_s : jsi::NativeState {
  std::optional<js_finalizer_t> external;
  std::optional<js_finalizer_t> wrap;
  std::vector<js_finalizer_t> finalizers;
  std::optional<js_type_tag_t> tag;

  ~js_native_state_s() override {
    if (wrap) wrap->finalize();

    for (auto it = finalizers.rbegin(); it != finalizers.rend(); it++) {
      it->finalize();
    }

    if (external) external->finalize();
  }
};

struct js_callback_s {
  js_env_t *env;
  js_function_cb cb;
  void *data;
};

struct js_callback_info_s {
  js_callback_t *callback;
  size_t argc;
  const jsi::Value *argv;
  const jsi::Value *receiver;
};

struct js_arraybuffer_backing_store_s {
  std::shared_ptr<jsi::MutableBuffer> buffer;

  // Buffers allocated by the engine rather than by us can't be shared, so the
  // backing store instead keeps the buffer itself alive for as long as the
  // environment it belongs to.
  js_env_t *env;
  std::optional<jsi::Value> owner;
  uint8_t *data;
  size_t len;

  intrusive_list_node_t list;
};

static const uint8_t js_threadsafe_function_idle = 0x0;
static const uint8_t js_threadsafe_function_running = 0x1;
static const uint8_t js_threadsafe_function_pending = 0x2;

struct js_threadsafe_queue_s {
  void **queue;
  size_t len;
  size_t capacity;
  size_t head;
  size_t tail;
  bool closed;
  uv_mutex_t lock;
};

struct js_threadsafe_function_s {
  std::optional<jsi::Value> function;
  js_env_t *env;
  uv_async_t async;
  js_threadsafe_queue_t queue;
  std::atomic<int> state;
  std::atomic<int> thread_count;
  void *context;
  js_finalize_cb finalize_cb;
  void *finalize_hint;
  js_threadsafe_function_cb cb;
  intrusive_list_node_t list;
};

struct js_buffer_s : jsi::MutableBuffer {
  uint8_t *bytes;
  size_t len;

  js_buffer_s(uint8_t *bytes, size_t len)
      : bytes(bytes),
        len(len) {}

  ~js_buffer_s() override {
    free(bytes);
  }

  size_t
  size() const override {
    return len;
  }

  uint8_t *
  data() override {
    return bytes;
  }
};

struct js_external_buffer_s : jsi::MutableBuffer {
  js_finalizer_t finalizer;
  size_t len;

  js_external_buffer_s(js_finalizer_t finalizer, size_t len)
      : finalizer(finalizer),
        len(len) {}

  ~js_external_buffer_s() override {
    finalizer.finalize();
  }

  size_t
  size() const override {
    return len;
  }

  uint8_t *
  data() override {
    return static_cast<uint8_t *>(finalizer.data);
  }
};

static const char *js__platform_identifier = "hermes";

static const char *js__platform_version = JS_HERMES_VERSION;

static const jsi::Value js__undefined = jsi::Value::undefined();

static inline jsi::Value &
js__value(js_value_t *value) {
  return *reinterpret_cast<jsi::Value *>(value);
}

static inline js_value_t *
js__handle(const jsi::Value *value) {
  return reinterpret_cast<js_value_t *>(const_cast<jsi::Value *>(value));
}

static inline js_value_t *
js__attach_to_handle_scope(js_env_t *env, js_handle_scope_t *scope, jsi::Value value) {
  scope->values.push_back(std::move(value));

  return js__handle(&scope->values.back());
}

static inline js_value_t *
js__attach(js_env_t *env, jsi::Value value) {
  return js__attach_to_handle_scope(env, env->scope, std::move(value));
}

static inline jsi::Object
js__object(js_env_t *env, js_value_t *value) {
  return js__value(value).getObject(env->rt());
}

static inline jsi::Value
js__copy(js_env_t *env, const jsi::Value &value) {
  return jsi::Value(env->rt(), value);
}

static inline std::vector<jsi::Value>
js__copy_values(js_env_t *env, js_value_t *const values[], size_t len) {
  std::vector<jsi::Value> copies;

  copies.reserve(len);

  for (size_t i = 0; i < len; i++) {
    copies.push_back(js__copy(env, js__value(values[i])));
  }

  return copies;
}

static inline jsi::Function
js__builtin(js_env_t *env, const char *name) {
  return env->rt().global().getPropertyAsFunction(env->rt(), name);
}

static inline jsi::Function
js__builtin(js_env_t *env, const char *object, const char *name) {
  return env->rt().global().getPropertyAsObject(env->rt(), object).getPropertyAsFunction(env->rt(), name);
}

static inline jsi::Value
js__string(js_env_t *env, const char *str) {
  return jsi::String::createFromUtf8(env->rt(), std::string(str));
}

static inline jsi::Value
js__string(js_env_t *env, const char *str, size_t len) {
  if (len == (size_t) -1) len = strlen(str);

  return jsi::String::createFromUtf8(env->rt(), reinterpret_cast<const uint8_t *>(str), len);
}

static inline jsi::Value
js__symbol(js_env_t *env, const char *description) {
  return js__builtin(env, "Symbol").call(env->rt(), js__string(env, description));
}

static inline int
js__error(js_env_t *env) {
  return env->exception ? js_pending_exception : js_uncaught_exception;
}

static void
js__on_uncaught_exception(js_env_t *env, jsi::Value error) {
  int err;

  if (env->terminated) {
    env->terminated = false;

    return;
  }

  if (env->callbacks.uncaught_exception) {
    js_handle_scope_t *scope;
    err = js_open_handle_scope(env, &scope);
    assert(err == 0);

    env->callbacks.uncaught_exception(env, js__attach(env, std::move(error)), env->callbacks.uncaught_exception_data);

    err = js_close_handle_scope(env, scope);
    assert(err == 0);
  } else {
    env->exception = std::move(error);
  }
}

static inline int
js__propagate_exception(js_env_t *env) {
  if (env->depth == 0) {
    auto error = std::move(*env->exception);

    env->exception.reset();

    js__on_uncaught_exception(env, std::move(error));
  }

  return js__error(env);
}

static jsi::Value
js__create_error(js_env_t *env, const char *type, const char *code, jsi::Value message) {
  auto &rt = env->rt();

  auto error = js__builtin(env, type).callAsConstructor(rt, message).getObject(rt);

  if (code) error.setProperty(rt, "code", js__string(env, code));

  return error;
}

static jsi::Value
js__error_from_exception(js_env_t *env, const jsi::JSIException &exception) {
  const char *message = exception.what();

  const char *type = strncmp(message, "Compiling JS failed", 19) == 0 ? "SyntaxError" : "Error";

  return js__create_error(env, type, NULL, js__string(env, message));
}

static void
js__run_microtasks(js_env_t *env) {
  if (env->terminated) return;

  while (true) {
    std::optional<jsi::Value> error;

    env->depth++;

    try {
      env->rt().drainMicrotasks();
    } catch (const jsi::JSError &exception) {
      error.emplace(js__copy(env, exception.value()));
    } catch (const jsi::JSIException &exception) {
      error.emplace(js__error_from_exception(env, exception));
    }

    env->depth--;

    if (!error) break;

    js__on_uncaught_exception(env, std::move(*error));

    if (env->exception) break;
  }
}

// Calls into the engine, which may run arbitrary JavaScript, translating any
// exception into a pending exception. Microtasks are drained once control
// returns to the outermost native frame.
template <typename F>
static inline int
js__call_into_javascript(js_env_t *env, F fn, bool always_checkpoint = false) {
  env->depth++;

  try {
    fn();
  } catch (const jsi::JSError &exception) {
    env->exception.emplace(js__copy(env, exception.value()));
  } catch (const jsi::JSIException &exception) {
    env->exception.emplace(js__error_from_exception(env, exception));
  }

  env->depth--;

  if (env->exception) return js__propagate_exception(env);

  if (env->depth == 0 || always_checkpoint) js__run_microtasks(env);

  return 0;
}

// Calls into the engine for an operation that never runs JavaScript, but that
// may still fail, such as when the engine rejects an argument.
template <typename F>
static inline int
js__call_into_engine(js_env_t *env, F fn) {
  try {
    fn();
  } catch (const jsi::JSError &exception) {
    env->exception.emplace(js__copy(env, exception.value()));
  } catch (const jsi::JSIException &exception) {
    env->exception.emplace(js__error_from_exception(env, exception));
  }

  if (env->exception) return js__propagate_exception(env);

  return 0;
}

static inline int
js__unsupported(js_env_t *env) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

extern "C" int
js_create_platform(uv_loop_t *loop, const js_platform_options_t *options, js_platform_t **result) {
  auto platform = new js_platform_t();

  platform->loop = loop;
  platform->options = options ? *options : js_platform_options_t{};

  *result = platform;

  return 0;
}

extern "C" int
js_destroy_platform(js_platform_t *platform) {
  delete platform;

  return 0;
}

extern "C" int
js_get_platform_identifier(js_platform_t *platform, const char **result) {
  *result = js__platform_identifier;

  return 0;
}

extern "C" int
js_get_platform_version(js_platform_t *platform, const char **result) {
  *result = js__platform_version;

  return 0;
}

extern "C" int
js_get_platform_limits(js_platform_t *platform, js_platform_limits_t *result) {
  result->arraybuffer_length = UINT32_MAX;
  result->string_length = 256 * 1024 * 1024 - 1;

  return 0;
}

extern "C" int
js_get_platform_loop(js_platform_t *platform, uv_loop_t **result) {
  *result = platform->loop;

  return 0;
}

static void
js__on_handle_close(uv_handle_t *handle) {
  auto env = static_cast<js_env_t *>(handle->data);

  if (--env->active_handles == 0) {
    delete env;
  }
}

static void
js__release_values(js_env_t *env) {
  intrusive_list_for_each(next, &env->references) {
    auto reference = intrusive_entry(next, js_ref_t, list);

    reference->value.reset();
    reference->weak.reset();
  }

  intrusive_list_for_each(next, &env->deferreds) {
    auto deferred = intrusive_entry(next, js_deferred_t, list);

    deferred->resolve.reset();
    deferred->reject.reset();
  }

  intrusive_list_for_each(next, &env->scripts) {
    auto script = intrusive_entry(next, js_script_t, list);

    script->id.reset();
    script->prepared.reset();
  }

  intrusive_list_for_each(next, &env->backing_stores) {
    auto backing_store = intrusive_entry(next, js_arraybuffer_backing_store_t, list);

    intrusive_list_remove(&env->backing_stores, &backing_store->list);

    backing_store->owner.reset();
    backing_store->env = NULL;
    backing_store->data = NULL;
    backing_store->len = 0;
  }

  intrusive_list_for_each(next, &env->threadsafe_functions) {
    auto function = intrusive_entry(next, js_threadsafe_function_t, list);

    function->function.reset();
  }
}

static void
js__close_env(js_env_t *env) {
  int err;

  err = js_close_handle_scope(env, env->root_scope);
  assert(err == 0);

  js__release_values(env);

  env->exception.reset();
  env->bindings.reset();
  env->default_module_id.reset();
  env->function_id_key.reset();

  // Tearing down the runtime finalizes every remaining object, which in turn
  // invokes any outstanding finalizers while the environment is still alive.
  env->runtime.reset();

  uv_close(reinterpret_cast<uv_handle_t *>(&env->teardown), js__on_handle_close);
}

static void
js__on_teardown(uv_async_t *handle) {
  auto env = static_cast<js_env_t *>(handle->data);

  if (env->refs == 0) js__close_env(env);
}

extern "C" int
js_create_env(uv_loop_t *loop, js_platform_t *platform, const js_env_options_t *options, js_env_t **result) {
  int err;

  auto gc = ::hermes::vm::GCConfig::Builder();

  if (options && options->memory_limit) {
    gc.withMaxHeapSize(static_cast<::hermes::vm::gcheapsize_t>(std::min<size_t>(options->memory_limit, std::numeric_limits<::hermes::vm::gcheapsize_t>::max())));
  }

  auto config = ::hermes::vm::RuntimeConfig::Builder()
                  .withGCConfig(gc.build())
                  .withMicrotaskQueue(true)
                  .withEnableAsyncGenerators(true)
                  .build();

  auto env = new js_env_t();

  env->loop = loop;
  env->active_handles = 1;

  env->platform = platform;

  env->refs = 0;
  env->depth = 0;

  env->runtime = facebook::hermes::makeHermesRuntime(config);

  env->external_memory = 0;

  env->terminated = false;
  env->destroying = false;

  intrusive_list_init(&env->teardown_queue.tasks);

  intrusive_list_init(&env->references);
  intrusive_list_init(&env->deferreds);
  intrusive_list_init(&env->scripts);
  intrusive_list_init(&env->backing_stores);
  intrusive_list_init(&env->threadsafe_functions);

  env->callbacks.uncaught_exception = NULL;
  env->callbacks.uncaught_exception_data = NULL;

  env->callbacks.unhandled_rejection = NULL;
  env->callbacks.unhandled_rejection_data = NULL;

  env->scope = NULL;

  err = js_open_handle_scope(env, &env->root_scope);
  assert(err == 0);

  env->bindings.emplace(jsi::Object(env->rt()));

  env->function_id_key.emplace(js__symbol(env, "__function_id"));

  err = uv_async_init(loop, &env->teardown, js__on_teardown);
  assert(err == 0);

  env->teardown.data = static_cast<void *>(env);

  uv_unref(reinterpret_cast<uv_handle_t *>(&env->teardown));

  *result = env;

  return 0;
}

extern "C" int
js_destroy_env(js_env_t *env) {
  env->destroying = true;

  intrusive_list_for_each(next, &env->teardown_queue.tasks) {
    auto task = intrusive_entry(next, js_teardown_task_t, list);

    if (task->type == js_teardown_task_t::js_deferred_teardown) {
      task->deferred.cb(&task->deferred.handle, task->data);
    } else {
      task->immediate.cb(task->data);

      intrusive_list_remove(&env->teardown_queue.tasks, &task->list);

      delete task;
    }
  }

  if (env->refs == 0) {
    js__close_env(env);
  } else {
    uv_ref(reinterpret_cast<uv_handle_t *>(&env->teardown));
  }

  return 0;
}

extern "C" int
js_on_uncaught_exception(js_env_t *env, js_uncaught_exception_cb cb, void *data) {
  env->callbacks.uncaught_exception = cb;
  env->callbacks.uncaught_exception_data = data;

  return 0;
}

// Hermes only tracks rejections through its Promise polyfill, which defers the
// report through `setTimeout()`, so the callback is accepted but never called.
extern "C" int
js_on_unhandled_rejection(js_env_t *env, js_unhandled_rejection_cb cb, void *data) {
  env->callbacks.unhandled_rejection = cb;
  env->callbacks.unhandled_rejection_data = data;

  return 0;
}

extern "C" int
js_on_dynamic_import(js_env_t *env, js_dynamic_import_cb cb, void *data) {
  return 0;
}

extern "C" int
js_get_env_loop(js_env_t *env, uv_loop_t **result) {
  *result = env->loop;

  return 0;
}

extern "C" int
js_get_env_platform(js_env_t *env, js_platform_t **result) {
  *result = env->platform;

  return 0;
}

extern "C" int
js_open_handle_scope(js_env_t *env, js_handle_scope_t **result) {
  // Allow continuing even with a pending exception

  auto scope = new js_handle_scope_t();

  scope->parent = env->scope;

  env->scope = scope;

  *result = scope;

  return 0;
}

extern "C" int
js_close_handle_scope(js_env_t *env, js_handle_scope_t *scope) {
  // Allow continuing even with a pending exception

  env->scope = scope->parent;

  delete scope;

  return 0;
}

extern "C" int
js_open_escapable_handle_scope(js_env_t *env, js_escapable_handle_scope_t **result) {
  // Allow continuing even with a pending exception

  auto scope = new js_escapable_handle_scope_t();

  scope->parent = env->scope;

  env->scope = scope;

  *result = scope;

  return 0;
}

extern "C" int
js_close_escapable_handle_scope(js_env_t *env, js_escapable_handle_scope_t *scope) {
  return js_close_handle_scope(env, scope);
}

extern "C" int
js_escape_handle(js_env_t *env, js_escapable_handle_scope_t *scope, js_value_t *escapee, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach_to_handle_scope(env, scope->parent, js__copy(env, js__value(escapee)));

  return 0;
}

// Hermes has no notion of realms, so a runtime only ever has the one context.
extern "C" int
js_create_context(js_env_t *env, js_context_t **result) {
  return js__unsupported(env);
}

extern "C" int
js_destroy_context(js_env_t *env, js_context_t *context) {
  return js__unsupported(env);
}

extern "C" int
js_enter_context(js_env_t *env, js_context_t *context) {
  return js__unsupported(env);
}

extern "C" int
js_exit_context(js_env_t *env, js_context_t *context) {
  return js__unsupported(env);
}

extern "C" int
js_get_bindings(js_env_t *env, js_value_t **result) {
  *result = js__attach(env, js__copy(env, *env->bindings));

  return 0;
}

static inline std::string
js__to_utf8(js_env_t *env, const jsi::Value &value) {
  return value.getString(env->rt()).utf8(env->rt());
}

extern "C" int
js_run_script(js_env_t *env, const char *file, size_t len, int offset, js_value_t *source, js_value_t **result) {
  if (env->exception) return js__error(env);

  auto url = file ? std::string(file, len == (size_t) -1 ? strlen(file) : len) : std::string();

  std::optional<jsi::Value> value;

  int err = js__call_into_javascript(env, [&] {
    auto buffer = std::make_shared<jsi::StringBuffer>(js__to_utf8(env, js__value(source)));

    value.emplace(env->rt().evaluateJavaScript(buffer, url));
  });

  if (err < 0) return err;

  if (result) *result = js__attach(env, std::move(*value));

  return 0;
}

extern "C" int
js_prepare_script(js_env_t *env, const char *file, size_t len, int offset, js_value_t *source, js_script_t **result) {
  if (env->exception) return js__error(env);

  auto url = file ? std::string(file, len == (size_t) -1 ? strlen(file) : len) : std::string();

  std::shared_ptr<const jsi::PreparedJavaScript> prepared;

  int err = js__call_into_engine(env, [&] {
    auto buffer = std::make_shared<jsi::StringBuffer>(js__to_utf8(env, js__value(source)));

    prepared = env->rt().prepareJavaScript(buffer, url);
  });

  if (err < 0) return err;

  auto script = new js_script_t();

  script->prepared = std::move(prepared);
  script->id.emplace(js__symbol(env, url.c_str()));
  script->name = strdup(url.c_str());

  intrusive_list_prepend(&env->scripts, &script->list);

  *result = script;

  return 0;
}

extern "C" int
js_prepare_script_with_code_cache(js_env_t *env, const char *file, size_t len, int offset, js_value_t *source, const void *cached_data, size_t cached_data_len, bool *cache_rejected, js_script_t **result) {
  if (cache_rejected) *cache_rejected = cached_data != NULL;

  return js_prepare_script(env, file, len, offset, source, result);
}

// Producing bytecode requires the Hermes compiler, which isn't part of the
// runtime library shipped with React Native.
extern "C" int
js_create_script_code_cache(js_env_t *env, js_script_t *script, void **data, size_t *len) {
  return js__unsupported(env);
}

extern "C" int
js_run_prepared_script(js_env_t *env, js_script_t *script, js_value_t **result) {
  if (env->exception) return js__error(env);

  std::optional<jsi::Value> value;

  int err = js__call_into_javascript(env, [&] {
    value.emplace(env->rt().evaluatePreparedJavaScript(script->prepared));
  });

  if (err < 0) return err;

  if (result) *result = js__attach(env, std::move(*value));

  return 0;
}

extern "C" int
js_delete_script(js_env_t *env, js_script_t *script) {
  // Allow continuing even with a pending exception

  intrusive_list_remove(&env->scripts, &script->list);

  free(script->name);

  delete script;

  return 0;
}

extern "C" int
js_get_script_name(js_env_t *env, js_script_t *script, const char **result) {
  // Allow continuing even with a pending exception

  *result = script->name;

  return 0;
}

extern "C" int
js_get_script_id(js_env_t *env, js_script_t *script, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach(env, js__copy(env, *script->id));

  return 0;
}

extern "C" int
js_on_script_dynamic_import(js_env_t *env, js_script_t *script, js_dynamic_import_cb cb, void *data) {
  return 0;
}

// Hermes compiles ES modules to CommonJS ahead of time and has no notion of
// module records at runtime.
extern "C" int
js_create_module(js_env_t *env, const char *name, size_t len, int offset, js_value_t *source, js_module_meta_cb cb, void *data, js_module_t **result) {
  return js__unsupported(env);
}

extern "C" int
js_create_module_with_code_cache(js_env_t *env, const char *name, size_t len, int offset, js_value_t *source, const void *cached_data, size_t cached_data_len, bool *cache_rejected, js_module_meta_cb cb, void *data, js_module_t **result) {
  if (cache_rejected) *cache_rejected = cached_data != NULL;

  return js_create_module(env, name, len, offset, source, cb, data, result);
}

extern "C" int
js_create_module_code_cache(js_env_t *env, js_module_t *module, void **data, size_t *len) {
  return js__unsupported(env);
}

extern "C" int
js_create_synthetic_module(js_env_t *env, const char *name, size_t len, js_value_t *const export_names[], size_t names_len, js_module_evaluate_cb cb, void *data, js_module_t **result) {
  return js__unsupported(env);
}

extern "C" int
js_delete_module(js_env_t *env, js_module_t *module) {
  return js__unsupported(env);
}

extern "C" int
js_get_module_name(js_env_t *env, js_module_t *module, const char **result) {
  return js__unsupported(env);
}

static inline const jsi::Value &
js__default_module_id(js_env_t *env) {
  if (!env->default_module_id) env->default_module_id.emplace(js__symbol(env, "module"));

  return *env->default_module_id;
}

extern "C" int
js_get_module_id(js_env_t *env, js_module_t *module, js_value_t **result) {
  return js__unsupported(env);
}

extern "C" int
js_get_default_module_id(js_env_t *env, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach(env, js__copy(env, js__default_module_id(env)));

  return 0;
}

extern "C" int
js_on_module_dynamic_import(js_env_t *env, js_module_t *module, js_dynamic_import_cb cb, void *data) {
  return 0;
}

extern "C" int
js_get_module_namespace(js_env_t *env, js_module_t *module, js_value_t **result) {
  return js__unsupported(env);
}

extern "C" int
js_set_module_export(js_env_t *env, js_module_t *module, js_value_t *name, js_value_t *value) {
  return js__unsupported(env);
}

extern "C" int
js_instantiate_module(js_env_t *env, js_module_t *module, js_module_resolve_cb cb, void *data) {
  return js__unsupported(env);
}

extern "C" int
js_run_module(js_env_t *env, js_module_t *module, js_value_t **result) {
  return js__unsupported(env);
}

static inline void
js__set_weak_reference(js_env_t *env, js_ref_t *reference) {
  if (!reference->value || !reference->value->isObject()) return;

  reference->weak.emplace(env->rt(), reference->value->getObject(env->rt()));

  reference->value.reset();
}

static inline void
js__clear_weak_reference(js_env_t *env, js_ref_t *reference) {
  if (!reference->weak) return;

  auto value = reference->weak->lock(env->rt());

  if (!value.isUndefined()) reference->value.emplace(std::move(value));

  reference->weak.reset();
}

extern "C" int
js_create_reference(js_env_t *env, js_value_t *value, uint32_t count, js_ref_t **result) {
  // Allow continuing even with a pending exception

  auto reference = new js_ref_t();

  reference->value.emplace(js__copy(env, js__value(value)));
  reference->count = count;

  if (reference->count == 0) js__set_weak_reference(env, reference);

  intrusive_list_prepend(&env->references, &reference->list);

  *result = reference;

  return 0;
}

extern "C" int
js_delete_reference(js_env_t *env, js_ref_t *reference) {
  // Allow continuing even with a pending exception

  intrusive_list_remove(&env->references, &reference->list);

  delete reference;

  return 0;
}

extern "C" int
js_reference_ref(js_env_t *env, js_ref_t *reference, uint32_t *result) {
  // Allow continuing even with a pending exception

  reference->count++;

  if (reference->count == 1) js__clear_weak_reference(env, reference);

  if (result) *result = reference->count;

  return 0;
}

extern "C" int
js_reference_unref(js_env_t *env, js_ref_t *reference, uint32_t *result) {
  // Allow continuing even with a pending exception

  if (reference->count > 0) {
    reference->count--;

    if (reference->count == 0) js__set_weak_reference(env, reference);
  }

  if (result) *result = reference->count;

  return 0;
}

extern "C" int
js_get_reference_value(js_env_t *env, js_ref_t *reference, js_value_t **result) {
  // Allow continuing even with a pending exception

  if (reference->value) {
    *result = js__attach(env, js__copy(env, *reference->value));
  } else if (reference->weak) {
    auto value = reference->weak->lock(env->rt());

    if (value.isUndefined()) *result = NULL;
    else *result = js__attach(env, std::move(value));
  } else {
    *result = NULL;
  }

  return 0;
}

static jsi::Value
js__on_function_call(js_callback_t *callback, jsi::Runtime &rt, const jsi::Value &receiver, const jsi::Value *argv, size_t argc) {
  int err;

  auto env = callback->env;

  js_callback_info_t callback_info = {
    .callback = callback,
    .argc = argc,
    .argv = argv,
    .receiver = &receiver,
  };

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  auto result = callback->cb(env, &callback_info);

  auto value = result ? js__copy(env, js__value(result)) : jsi::Value::undefined();

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  if (env->exception) {
    auto error = std::move(*env->exception);

    env->exception.reset();

    throw jsi::JSError(std::move(error), "", "");
  }

  return value;
}

static inline jsi::Function
js__create_function(js_env_t *env, const char *name, size_t len, js_function_cb cb, void *data) {
  auto &rt = env->rt();

  auto callback = std::make_shared<js_callback_t>(js_callback_t{env, cb, data});

  auto id = name
              ? jsi::PropNameID::forUtf8(rt, reinterpret_cast<const uint8_t *>(name), len == (size_t) -1 ? strlen(name) : len)
              : jsi::PropNameID::forAscii(rt, "");

  return jsi::Function::createFromHostFunction(
    rt,
    id,
    0,
    [callback](jsi::Runtime &rt, const jsi::Value &receiver, const jsi::Value *argv, size_t argc) {
      return js__on_function_call(callback.get(), rt, receiver, argv, argc);
    }
  );
}

// Host functions are never constructors in Hermes, so a class can't be backed
// by a native constructor without a JavaScript shim.
extern "C" int
js_define_class(js_env_t *env, const char *name, size_t len, js_function_cb constructor, void *data, js_property_descriptor_t const properties[], size_t properties_len, js_value_t **result) {
  if (env->exception) return js__error(env);

  return js__unsupported(env);
}

extern "C" int
js_define_properties(js_env_t *env, js_value_t *object, js_property_descriptor_t const properties[], size_t properties_len) {
  if (env->exception) return js__error(env);

  auto &rt = env->rt();

  return js__call_into_javascript(env, [&] {
    auto define_property = js__builtin(env, "Object", "defineProperty");

    for (size_t i = 0; i < properties_len; i++) {
      const js_property_descriptor_t *property = &properties[i];

      auto descriptor = jsi::Object(rt);

      descriptor.setProperty(rt, "enumerable", (property->attributes & js_enumerable) != 0);
      descriptor.setProperty(rt, "configurable", (property->attributes & js_configurable) != 0);

      if (property->getter || property->setter) {
        if (property->getter) {
          descriptor.setProperty(rt, "get", js__create_function(env, "get", -1, property->getter, property->data));
        }

        if (property->setter) {
          descriptor.setProperty(rt, "set", js__create_function(env, "set", -1, property->setter, property->data));
        }
      } else {
        descriptor.setProperty(rt, "writable", (property->attributes & js_writable) != 0);

        if (property->method) {
          descriptor.setProperty(rt, "value", js__create_function(env, NULL, 0, property->method, property->data));
        } else {
          descriptor.setProperty(rt, "value", js__value(property->value));
        }
      }

      define_property.call(rt, js__value(object), js__value(property->name), descriptor);
    }
  });
}

static inline std::shared_ptr<js_native_state_t>
js__get_native_state(js_env_t *env, const jsi::Object &object) {
  auto &rt = env->rt();

  if (!object.hasNativeState(rt)) return nullptr;

  return std::dynamic_pointer_cast<js_native_state_t>(object.getNativeState(rt));
}

static inline std::shared_ptr<js_native_state_t>
js__get_or_create_native_state(js_env_t *env, const jsi::Object &object) {
  auto state = js__get_native_state(env, object);

  if (state) return state;

  state = std::make_shared<js_native_state_t>();

  object.setNativeState(env->rt(), state);

  return state;
}

extern "C" int
js_wrap(js_env_t *env, js_value_t *object, void *data, js_finalize_cb finalize_cb, void *finalize_hint, js_ref_t **result) {
  if (env->exception) return js__error(env);

  int err;

  auto local = js__object(env, object);

  std::shared_ptr<js_native_state_t> state;

  err = js__call_into_engine(env, [&] {
    state = js__get_or_create_native_state(env, local);
  });

  if (err < 0) return err;

  if (state->wrap) {
    err = js_throw_error(env, NULL, "Object is already wrapped");
    assert(err == 0);

    return js__error(env);
  }

  state->wrap = js_finalizer_t{env, data, finalize_cb, finalize_hint};

  if (result) return js_create_reference(env, object, 0, result);

  return 0;
}

static inline int
js__get_wrap(js_env_t *env, js_value_t *object, std::shared_ptr<js_native_state_t> &result) {
  int err;

  auto state = js__get_native_state(env, js__object(env, object));

  if (state == nullptr || !state->wrap) {
    err = js_throw_type_error(env, NULL, "Object is not wrapped");
    assert(err == 0);

    return js__error(env);
  }

  result = std::move(state);

  return 0;
}

extern "C" int
js_unwrap(js_env_t *env, js_value_t *object, void **result) {
  if (env->exception) return js__error(env);

  int err;

  std::shared_ptr<js_native_state_t> state;
  err = js__get_wrap(env, object, state);
  if (err < 0) return err;

  *result = state->wrap->data;

  return 0;
}

extern "C" int
js_remove_wrap(js_env_t *env, js_value_t *object, void **result) {
  if (env->exception) return js__error(env);

  int err;

  std::shared_ptr<js_native_state_t> state;
  err = js__get_wrap(env, object, state);
  if (err < 0) return err;

  if (result) *result = state->wrap->data;

  state->wrap.reset();

  return 0;
}

// Hermes can't attach native state to a proxy, so a delegate couldn't carry
// its finalizer, wraps, or type tags.
extern "C" int
js_create_delegate(js_env_t *env, const js_delegate_callbacks_t *callbacks, void *data, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result) {
  return js__unsupported(env);
}

extern "C" int
js_add_finalizer(js_env_t *env, js_value_t *object, void *data, js_finalize_cb finalize_cb, void *finalize_hint, js_ref_t **result) {
  // Allow continuing even with a pending exception

  int err;

  auto local = js__object(env, object);

  std::shared_ptr<js_native_state_t> state;

  err = js__call_into_engine(env, [&] {
    state = js__get_or_create_native_state(env, local);
  });

  if (err < 0) return err;

  state->finalizers.push_back(js_finalizer_t{env, data, finalize_cb, finalize_hint});

  if (result) return js_create_reference(env, object, 0, result);

  return 0;
}

extern "C" int
js_add_type_tag(js_env_t *env, js_value_t *object, const js_type_tag_t *tag) {
  if (env->exception) return js__error(env);

  int err;

  auto local = js__object(env, object);

  std::shared_ptr<js_native_state_t> state;

  err = js__call_into_engine(env, [&] {
    state = js__get_or_create_native_state(env, local);
  });

  if (err < 0) return err;

  if (state->tag) {
    err = js_throw_error(env, NULL, "Object is already type tagged");
    assert(err == 0);

    return js__error(env);
  }

  state->tag = *tag;

  return 0;
}

extern "C" int
js_check_type_tag(js_env_t *env, js_value_t *object, const js_type_tag_t *tag, bool *result) {
  if (env->exception) return js__error(env);

  auto state = js__get_native_state(env, js__object(env, object));

  *result = state && state->tag && state->tag->lower == tag->lower && state->tag->upper == tag->upper;

  return 0;
}

extern "C" int
js_create_int32(js_env_t *env, int32_t value, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach(env, jsi::Value(static_cast<double>(value)));

  return 0;
}

extern "C" int
js_create_uint32(js_env_t *env, uint32_t value, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach(env, jsi::Value(static_cast<double>(value)));

  return 0;
}

extern "C" int
js_create_int64(js_env_t *env, int64_t value, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach(env, jsi::Value(static_cast<double>(value)));

  return 0;
}

extern "C" int
js_create_double(js_env_t *env, double value, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach(env, jsi::Value(value));

  return 0;
}

extern "C" int
js_create_bigint_int64(js_env_t *env, int64_t value, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach(env, jsi::BigInt::fromInt64(env->rt(), value));

  return 0;
}

extern "C" int
js_create_bigint_uint64(js_env_t *env, uint64_t value, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach(env, jsi::BigInt::fromUint64(env->rt(), value));

  return 0;
}

// JSI only constructs BigInts from 64-bit integers, so wider values go through
// their decimal representation, which `BigInt()` accepts with a sign.
extern "C" int
js_create_bigint_words(js_env_t *env, int sign, const uint64_t *words, size_t len, js_value_t **result) {
  if (env->exception) return js__error(env);

  std::vector<uint64_t> remaining(words, words + len);

  std::string digits;

  while (!remaining.empty() && remaining.back() == 0) remaining.pop_back();

  while (!remaining.empty()) {
    unsigned __int128 remainder = 0;

    for (size_t i = remaining.size(); i-- > 0;) {
      unsigned __int128 current = (remainder << 64) | remaining[i];

      remaining[i] = static_cast<uint64_t>(current / 10000000000000000000ull);
      remainder = current % 10000000000000000000ull;
    }

    while (!remaining.empty() && remaining.back() == 0) remaining.pop_back();

    char chunk[20];
    snprintf(chunk, sizeof(chunk), remaining.empty() ? "%llu" : "%019llu", static_cast<unsigned long long>(remainder));

    digits.insert(0, chunk);
  }

  if (digits.empty()) digits = "0";
  else if (sign) digits.insert(0, "-");

  std::optional<jsi::Value> value;

  int err = js__call_into_engine(env, [&] {
    value.emplace(js__builtin(env, "BigInt").call(env->rt(), js__string(env, digits.c_str())));
  });

  if (err < 0) return err;

  *result = js__attach(env, std::move(*value));

  return 0;
}

static inline int
js__check_string_length(js_env_t *env, size_t len) {
  int err;

  if (len == (size_t) -1 || len < 256 * 1024 * 1024) return 0;

  err = js_throw_range_error(env, NULL, "Invalid string length");
  assert(err == 0);

  return js__error(env);
}

extern "C" int
js_create_string_utf8(js_env_t *env, const utf8_t *str, size_t len, js_value_t **result) {
  // Allow continuing even with a pending exception

  int err;

  err = js__check_string_length(env, len);
  if (err < 0) return err;

  if (len == (size_t) -1) len = strlen(reinterpret_cast<const char *>(str));

  *result = js__attach(env, jsi::String::createFromUtf8(env->rt(), str, len));

  return 0;
}

extern "C" int
js_create_string_utf16le(js_env_t *env, const utf16_t *str, size_t len, js_value_t **result) {
  // Allow continuing even with a pending exception

  int err;

  err = js__check_string_length(env, len);
  if (err < 0) return err;

  if (len == (size_t) -1) {
    len = 0;

    while (str[len] != 0) len++;
  }

  *result = js__attach(env, jsi::String::createFromUtf16(env->rt(), reinterpret_cast<const char16_t *>(str), len));

  return 0;
}

extern "C" int
js_create_string_latin1(js_env_t *env, const latin1_t *str, size_t len, js_value_t **result) {
  // Allow continuing even with a pending exception

  int err;

  err = js__check_string_length(env, len);
  if (err < 0) return err;

  if (len == (size_t) -1) len = strlen(reinterpret_cast<const char *>(str));

  auto utf16 = std::u16string(utf16_length_from_latin1(str, len), u'\0');

  latin1_convert_to_utf16le(str, len, reinterpret_cast<utf16_t *>(utf16.data()));

  *result = js__attach(env, jsi::String::createFromUtf16(env->rt(), utf16));

  return 0;
}

extern "C" int
js_create_external_string_utf8(js_env_t *env, utf8_t *str, size_t len, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result, bool *copied) {
  // Allow continuing even with a pending exception

  int err;
  err = js_create_string_utf8(env, str, len, result);
  if (err < 0) return err;

  if (copied) *copied = true;

  if (finalize_cb) finalize_cb(env, str, finalize_hint);

  return 0;
}

extern "C" int
js_create_external_string_utf16le(js_env_t *env, utf16_t *str, size_t len, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result, bool *copied) {
  // Allow continuing even with a pending exception

  int err;
  err = js_create_string_utf16le(env, str, len, result);
  if (err < 0) return err;

  if (copied) *copied = true;

  if (finalize_cb) finalize_cb(env, str, finalize_hint);

  return 0;
}

extern "C" int
js_create_external_string_latin1(js_env_t *env, latin1_t *str, size_t len, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result, bool *copied) {
  // Allow continuing even with a pending exception

  int err;
  err = js_create_string_latin1(env, str, len, result);
  if (err < 0) return err;

  if (copied) *copied = true;

  if (finalize_cb) finalize_cb(env, str, finalize_hint);

  return 0;
}

extern "C" int
js_create_property_key_utf8(js_env_t *env, const utf8_t *str, size_t len, js_value_t **result) {
  return js_create_string_utf8(env, str, len, result);
}

extern "C" int
js_create_property_key_utf16le(js_env_t *env, const utf16_t *str, size_t len, js_value_t **result) {
  return js_create_string_utf16le(env, str, len, result);
}

extern "C" int
js_create_property_key_latin1(js_env_t *env, const latin1_t *str, size_t len, js_value_t **result) {
  return js_create_string_latin1(env, str, len, result);
}

extern "C" int
js_create_symbol(js_env_t *env, js_value_t *description, js_value_t **result) {
  // Allow continuing even with a pending exception

  auto &rt = env->rt();

  auto symbol = description
                  ? js__builtin(env, "Symbol").call(rt, js__value(description))
                  : js__builtin(env, "Symbol").call(rt);

  *result = js__attach(env, std::move(symbol));

  return 0;
}

extern "C" int
js_symbol_for(js_env_t *env, const char *description, size_t len, js_value_t **result) {
  // Allow continuing even with a pending exception

  auto symbol = js__builtin(env, "Symbol", "for").call(env->rt(), js__string(env, description, len));

  *result = js__attach(env, std::move(symbol));

  return 0;
}

extern "C" int
js_create_object(js_env_t *env, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach(env, jsi::Object(env->rt()));

  return 0;
}

extern "C" int
js_create_object_with_prototype(js_env_t *env, js_value_t *prototype, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach(env, jsi::Object::create(env->rt(), js__value(prototype)));

  return 0;
}

extern "C" int
js_create_object_with_properties(js_env_t *env, js_value_t *prototype, js_value_t *const property_names[], js_value_t *const property_values[], size_t property_count, js_value_t **result) {
  // Allow continuing even with a pending exception

  auto &rt = env->rt();

  auto object = jsi::Object::create(rt, js__value(prototype));

  for (size_t i = 0; i < property_count; i++) {
    object.setProperty(rt, js__value(property_names[i]), js__value(property_values[i]));
  }

  *result = js__attach(env, std::move(object));

  return 0;
}

extern "C" int
js_create_function(js_env_t *env, const char *name, size_t len, js_function_cb cb, void *data, js_value_t **result) {
  if (env->exception) return js__error(env);

  *result = js__attach(env, js__create_function(env, name, len, cb, data));

  return 0;
}

// Determines whether an argument name is an identifier, so that it can't break
// out of the parameter list it's spliced into. Any code point beyond ASCII is
// left for the engine to validate.
static inline bool
js__is_identifier(const std::string &name) {
  if (name.empty()) return false;

  for (size_t i = 0; i < name.size(); i++) {
    unsigned char c = name[i];

    if (c >= 0x80 || c == '_' || c == '$' || isalpha(c)) continue;

    if (i > 0 && isdigit(c)) continue;

    return false;
  }

  return true;
}

extern "C" int
js_compile_function(js_env_t *env, const char *name, size_t name_len, const char *file, size_t file_len, js_value_t *const args[], size_t args_len, int offset, js_value_t *source, js_value_t **result) {
  if (env->exception) return js__error(env);

  auto &rt = env->rt();

  auto url = file ? std::string(file, file_len == (size_t) -1 ? strlen(file) : file_len) : std::string();

  std::optional<jsi::Value> function;

  int err;

  for (size_t i = 0; i < args_len; i++) {
    if (!js__is_identifier(js__to_utf8(env, js__value(args[i])))) {
      err = js_throw_syntax_error(env, NULL, "Invalid argument name");
      assert(err == 0);

      return js__error(env);
    }
  }

  err = js__call_into_engine(env, [&] {
    std::string code = "(function (";

    for (size_t i = 0; i < args_len; i++) {
      if (i > 0) code += ", ";

      code += js__to_utf8(env, js__value(args[i]));
    }

    code += ") {\n";
    code += js__to_utf8(env, js__value(source));
    code += "\n})";

    function.emplace(rt.evaluateJavaScript(std::make_shared<jsi::StringBuffer>(std::move(code)), url));

    auto define_property = js__builtin(env, "Object", "defineProperty");

    auto descriptor = jsi::Object(rt);

    if (name) {
      descriptor.setProperty(rt, "value", js__string(env, name, name_len));

      define_property.call(rt, *function, "name", descriptor);
    }

    descriptor.setProperty(rt, "value", js__symbol(env, url.c_str()));

    define_property.call(rt, *function, *env->function_id_key, descriptor);
  });

  if (err < 0) return err;

  *result = js__attach(env, std::move(*function));

  return 0;
}

extern "C" int
js_compile_function_with_code_cache(js_env_t *env, const char *name, size_t name_len, const char *file, size_t file_len, js_value_t *const args[], size_t args_len, int offset, js_value_t *source, const void *cached_data, size_t cached_data_len, bool *cache_rejected, js_value_t **result) {
  if (cache_rejected) *cache_rejected = cached_data != NULL;

  return js_compile_function(env, name, name_len, file, file_len, args, args_len, offset, source, result);
}

extern "C" int
js_create_function_code_cache(js_env_t *env, js_value_t *function, void **data, size_t *len) {
  return js__unsupported(env);
}

extern "C" int
js_create_function_with_source(js_env_t *env, const char *name, size_t name_len, const char *file, size_t file_len, js_value_t *const args[], size_t args_len, int offset, js_value_t *source, js_value_t **result) {
  return js_compile_function(env, name, name_len, file, file_len, args, args_len, offset, source, result);
}

extern "C" int
js_on_function_dynamic_import(js_env_t *env, js_value_t *function, js_dynamic_import_cb cb, void *data) {
  return 0;
}

extern "C" int
js_create_typed_function(js_env_t *env, const char *name, size_t len, js_function_cb cb, const js_callback_signature_t *signature, const void *address, void *data, js_value_t **result) {
  return js_create_function(env, name, len, cb, data, result);
}

extern "C" int
js_get_function_id(js_env_t *env, js_value_t *function, js_value_t **result) {
  // Allow continuing even with a pending exception

  auto &rt = env->rt();

  auto id = js__object(env, function).getProperty(rt, *env->function_id_key);

  if (!id.isSymbol()) id = js__copy(env, js__default_module_id(env));

  *result = js__attach(env, std::move(id));

  return 0;
}

extern "C" int
js_create_array(js_env_t *env, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach(env, jsi::Array(env->rt(), 0));

  return 0;
}

extern "C" int
js_create_array_with_length(js_env_t *env, size_t len, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach(env, jsi::Array(env->rt(), len));

  return 0;
}

extern "C" int
js_create_array_with_elements(js_env_t *env, js_value_t *const elements[], size_t element_count, js_value_t **result) {
  // Allow continuing even with a pending exception

  auto &rt = env->rt();

  auto array = jsi::Array(rt, element_count);

  for (size_t i = 0; i < element_count; i++) {
    array.setValueAtIndex(rt, i, js__value(elements[i]));
  }

  *result = js__attach(env, std::move(array));

  return 0;
}

extern "C" int
js_create_external(js_env_t *env, void *data, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result) {
  // Allow continuing even with a pending exception

  auto &rt = env->rt();

  auto object = jsi::Object(rt);

  auto state = std::make_shared<js_native_state_t>();

  state->external = js_finalizer_t{env, data, finalize_cb, finalize_hint};

  object.setNativeState(rt, state);

  *result = js__attach(env, std::move(object));

  return 0;
}

extern "C" int
js_create_date(js_env_t *env, double time, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach(env, js__builtin(env, "Date").callAsConstructor(env->rt(), time));

  return 0;
}

static inline int
js__create_error(js_env_t *env, const char *type, js_value_t *code, js_value_t *message, js_value_t **result) {
  auto &rt = env->rt();

  auto error = js__builtin(env, type).callAsConstructor(rt, js__value(message)).getObject(rt);

  if (code) error.setProperty(rt, "code", js__value(code));

  *result = js__attach(env, std::move(error));

  return 0;
}

extern "C" int
js_create_error(js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  // Allow continuing even with a pending exception

  return js__create_error(env, "Error", code, message, result);
}

extern "C" int
js_create_type_error(js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  // Allow continuing even with a pending exception

  return js__create_error(env, "TypeError", code, message, result);
}

extern "C" int
js_create_range_error(js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  // Allow continuing even with a pending exception

  return js__create_error(env, "RangeError", code, message, result);
}

extern "C" int
js_create_syntax_error(js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  // Allow continuing even with a pending exception

  return js__create_error(env, "SyntaxError", code, message, result);
}

extern "C" int
js_create_reference_error(js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  // Allow continuing even with a pending exception

  return js__create_error(env, "ReferenceError", code, message, result);
}

extern "C" int
js_get_error_location(js_env_t *env, js_value_t *error, js_error_location_t *result) {
  result->name = js__handle(&js__undefined);
  result->source = js__handle(&js__undefined);
  result->line = 0;
  result->column_start = -1;
  result->column_end = -1;

  return 0;
}

extern "C" int
js_create_promise(js_env_t *env, js_deferred_t **deferred, js_value_t **promise) {
  // Allow continuing even with a pending exception

  auto &rt = env->rt();

  auto result = new js_deferred_t();

  auto executor = jsi::Function::createFromHostFunction(
    rt,
    jsi::PropNameID::forAscii(rt, "executor"),
    2,
    [env, result](jsi::Runtime &rt, const jsi::Value &receiver, const jsi::Value *argv, size_t argc) {
      result->resolve.emplace(js__copy(env, argv[0]));
      result->reject.emplace(js__copy(env, argv[1]));

      return jsi::Value::undefined();
    }
  );

  auto value = js__builtin(env, "Promise").callAsConstructor(rt, executor);

  intrusive_list_prepend(&env->deferreds, &result->list);

  *deferred = result;
  *promise = js__attach(env, std::move(value));

  return 0;
}

static inline int
js__conclude_deferred(js_env_t *env, js_deferred_t *deferred, const jsi::Value &function, js_value_t *resolution) {
  int err = js__call_into_javascript(env, [&] {
    function.getObject(env->rt()).getFunction(env->rt()).call(env->rt(), js__value(resolution));
  });

  intrusive_list_remove(&env->deferreds, &deferred->list);

  delete deferred;

  return err;
}

extern "C" int
js_resolve_deferred(js_env_t *env, js_deferred_t *deferred, js_value_t *resolution) {
  // Allow continuing even with a pending exception

  return js__conclude_deferred(env, deferred, *deferred->resolve, resolution);
}

extern "C" int
js_reject_deferred(js_env_t *env, js_deferred_t *deferred, js_value_t *resolution) {
  // Allow continuing even with a pending exception

  return js__conclude_deferred(env, deferred, *deferred->reject, resolution);
}

// Promises are implemented by a JavaScript polyfill in Hermes whose internal
// state is obfuscated at build time.
extern "C" int
js_get_promise_state(js_env_t *env, js_value_t *promise, js_promise_state_t *result) {
  return js__unsupported(env);
}

extern "C" int
js_get_promise_result(js_env_t *env, js_value_t *promise, js_value_t **result) {
  return js__unsupported(env);
}

static inline int
js__create_arraybuffer(js_env_t *env, std::shared_ptr<jsi::MutableBuffer> buffer, js_value_t **result) {
  std::optional<jsi::Value> arraybuffer;

  int err = js__call_into_engine(env, [&] {
    arraybuffer.emplace(jsi::ArrayBuffer(env->rt(), std::move(buffer)));
  });

  if (err < 0) return err;

  *result = js__attach(env, std::move(*arraybuffer));

  return 0;
}

static inline int
js__allocate_arraybuffer(js_env_t *env, size_t len, bool zero, void **data, js_value_t **result) {
  int err;

  if (len > UINT32_MAX) {
    err = js_throw_range_error(env, NULL, "Array buffer allocation failed");
    assert(err == 0);

    return js__error(env);
  }

  auto bytes = static_cast<uint8_t *>(zero ? calloc(len ? len : 1, 1) : malloc(len ? len : 1));

  if (bytes == NULL) {
    err = js_throw_range_error(env, NULL, "Array buffer allocation failed");
    assert(err == 0);

    return js__error(env);
  }

  err = js__create_arraybuffer(env, std::make_shared<js_buffer_s>(bytes, len), result);
  if (err < 0) return err;

  if (data) *data = bytes;

  return 0;
}

extern "C" int
js_create_arraybuffer(js_env_t *env, size_t len, void **data, js_value_t **result) {
  if (env->exception) return js__error(env);

  return js__allocate_arraybuffer(env, len, true, data, result);
}

extern "C" int
js_create_arraybuffer_with_backing_store(js_env_t *env, js_arraybuffer_backing_store_t *backing_store, void **data, size_t *len, js_value_t **result) {
  if (env->exception) return js__error(env);

  int err;

  if (backing_store->buffer == nullptr) return js__unsupported(env);

  err = js__create_arraybuffer(env, backing_store->buffer, result);
  if (err < 0) return err;

  if (data) *data = backing_store->buffer->data();

  if (len) *len = backing_store->buffer->size();

  return 0;
}

extern "C" int
js_create_unsafe_arraybuffer(js_env_t *env, size_t len, void **data, js_value_t **result) {
  if (env->exception) return js__error(env);

  return js__allocate_arraybuffer(env, len, false, data, result);
}

extern "C" int
js_create_external_arraybuffer(js_env_t *env, void *data, size_t len, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result) {
  if (env->exception) return js__error(env);

  auto buffer = std::make_shared<js_external_buffer_s>(js_finalizer_t{env, data, finalize_cb, finalize_hint}, len);

  return js__create_arraybuffer(env, std::move(buffer), result);
}

// Hermes exposes no way of detaching an ArrayBuffer through JSI.
extern "C" int
js_detach_arraybuffer(js_env_t *env, js_value_t *arraybuffer) {
  return js__unsupported(env);
}

extern "C" int
js_get_arraybuffer_backing_store(js_env_t *env, js_value_t *arraybuffer, js_arraybuffer_backing_store_t **result) {
  // Allow continuing even with a pending exception

  auto &rt = env->rt();

  auto local = js__object(env, arraybuffer).getArrayBuffer(rt);

  auto backing_store = new js_arraybuffer_backing_store_t();

  backing_store->env = NULL;
  backing_store->buffer = local.tryGetMutableBuffer(rt);
  backing_store->data = local.data(rt);
  backing_store->len = local.size(rt);

  if (backing_store->buffer == nullptr) {
    backing_store->env = env;
    backing_store->owner.emplace(js__copy(env, js__value(arraybuffer)));

    intrusive_list_prepend(&env->backing_stores, &backing_store->list);
  }

  *result = backing_store;

  return 0;
}

// SharedArrayBuffer isn't implemented by Hermes.
extern "C" int
js_create_sharedarraybuffer(js_env_t *env, size_t len, void **data, js_value_t **result) {
  return js__unsupported(env);
}

extern "C" int
js_create_sharedarraybuffer_with_backing_store(js_env_t *env, js_arraybuffer_backing_store_t *backing_store, void **data, size_t *len, js_value_t **result) {
  return js__unsupported(env);
}

extern "C" int
js_create_unsafe_sharedarraybuffer(js_env_t *env, size_t len, void **data, js_value_t **result) {
  return js__unsupported(env);
}

extern "C" int
js_create_external_sharedarraybuffer(js_env_t *env, void *data, size_t len, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result) {
  return js__unsupported(env);
}

extern "C" int
js_get_sharedarraybuffer_backing_store(js_env_t *env, js_value_t *sharedarraybuffer, js_arraybuffer_backing_store_t **result) {
  return js__unsupported(env);
}

extern "C" int
js_release_arraybuffer_backing_store(js_env_t *env, js_arraybuffer_backing_store_t *backing_store) {
  // Allow continuing even with a pending exception

  if (backing_store->env) {
    intrusive_list_remove(&backing_store->env->backing_stores, &backing_store->list);
  }

  delete backing_store;

  return 0;
}

static const js_typedarray_type_t js__typedarray_types[] = {
  js_int8array,
  js_uint8array,
  js_uint8clampedarray,
  js_int16array,
  js_uint16array,
  js_int32array,
  js_uint32array,
  js_float16array,
  js_float32array,
  js_float64array,
  js_bigint64array,
  js_biguint64array,
};

static inline const char *
js__typedarray_name(js_typedarray_type_t type) {
  switch (type) {
  case js_int8array:
    return "Int8Array";
  case js_uint8array:
    return "Uint8Array";
  case js_uint8clampedarray:
    return "Uint8ClampedArray";
  case js_int16array:
    return "Int16Array";
  case js_uint16array:
    return "Uint16Array";
  case js_int32array:
    return "Int32Array";
  case js_uint32array:
    return "Uint32Array";
  case js_float16array:
    return "Float16Array";
  case js_float32array:
    return "Float32Array";
  case js_float64array:
    return "Float64Array";
  case js_bigint64array:
    return "BigInt64Array";
  case js_biguint64array:
    return "BigUint64Array";
  }

  return NULL;
}

extern "C" int
js_create_typedarray(js_env_t *env, js_typedarray_type_t type, size_t len, js_value_t *arraybuffer, size_t offset, js_value_t **result) {
  if (env->exception) return js__error(env);

  auto &rt = env->rt();

  // Float16Array isn't implemented by Hermes.
  if (type == js_float16array) return js__unsupported(env);

  std::optional<jsi::Value> typedarray;

  int err = js__call_into_engine(env, [&] {
    typedarray.emplace(js__builtin(env, js__typedarray_name(type)).callAsConstructor(rt, js__value(arraybuffer), static_cast<double>(offset), static_cast<double>(len)));
  });

  if (err < 0) return err;

  *result = js__attach(env, std::move(*typedarray));

  return 0;
}

extern "C" int
js_create_dataview(js_env_t *env, size_t len, js_value_t *arraybuffer, size_t offset, js_value_t **result) {
  if (env->exception) return js__error(env);

  auto &rt = env->rt();

  std::optional<jsi::Value> dataview;

  int err = js__call_into_engine(env, [&] {
    dataview.emplace(js__builtin(env, "DataView").callAsConstructor(rt, js__value(arraybuffer), static_cast<double>(offset), static_cast<double>(len)));
  });

  if (err < 0) return err;

  *result = js__attach(env, std::move(*dataview));

  return 0;
}

static inline bool
js__to_boolean(js_env_t *env, const jsi::Value &value) {
  auto &rt = env->rt();

  if (value.isUndefined() || value.isNull()) return false;

  if (value.isBool()) return value.getBool();

  if (value.isNumber()) {
    double number = value.getNumber();

    return number != 0 && !std::isnan(number);
  }

  if (value.isString()) return value.getString(rt).length(rt) > 0;

  if (value.isBigInt()) return value.getBigInt(rt).toString(rt).utf8(rt) != "0";

  return true;
}

extern "C" int
js_coerce_to_boolean(js_env_t *env, js_value_t *value, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach(env, jsi::Value(js__to_boolean(env, js__value(value))));

  return 0;
}

static inline int
js__coerce(js_env_t *env, const char *type, js_value_t *value, js_value_t **result) {
  std::optional<jsi::Value> coerced;

  int err = js__call_into_javascript(env, [&] {
    coerced.emplace(js__builtin(env, type).call(env->rt(), js__value(value)));
  });

  if (err < 0) return err;

  *result = js__attach(env, std::move(*coerced));

  return 0;
}

extern "C" int
js_coerce_to_number(js_env_t *env, js_value_t *value, js_value_t **result) {
  if (env->exception) return js__error(env);

  return js__coerce(env, "Number", value, result);
}

extern "C" int
js_coerce_to_string(js_env_t *env, js_value_t *value, js_value_t **result) {
  if (env->exception) return js__error(env);

  int err;

  // Unlike the ToString operation, `String()` converts symbols to their
  // description rather than throwing.
  if (js__value(value).isSymbol()) {
    err = js_throw_type_error(env, NULL, "Cannot convert a Symbol value to a string");
    assert(err == 0);

    return js__error(env);
  }

  return js__coerce(env, "String", value, result);
}

extern "C" int
js_coerce_to_object(js_env_t *env, js_value_t *value, js_value_t **result) {
  if (env->exception) return js__error(env);

  int err;

  auto &local = js__value(value);

  if (local.isUndefined() || local.isNull()) {
    err = js_throw_type_error(env, NULL, "Cannot convert undefined or null to object");
    assert(err == 0);

    return js__error(env);
  }

  return js__coerce(env, "Object", value, result);
}

extern "C" int
js_typeof(js_env_t *env, js_value_t *value, js_value_type_t *result) {
  // Allow continuing even with a pending exception

  auto &rt = env->rt();

  auto &local = js__value(value);

  if (local.isUndefined()) *result = js_undefined;
  else if (local.isNull()) *result = js_null;
  else if (local.isBool()) *result = js_boolean;
  else if (local.isNumber()) *result = js_number;
  else if (local.isString()) *result = js_string;
  else if (local.isSymbol()) *result = js_symbol;
  else if (local.isBigInt()) *result = js_bigint;
  else {
    auto object = local.getObject(rt);

    if (object.isFunction(rt)) *result = js_function;
    else {
      auto state = js__get_native_state(env, object);

      *result = state && state->external ? js_external : js_object;
    }
  }

  return 0;
}

extern "C" int
js_instanceof(js_env_t *env, js_value_t *object, js_value_t *constructor, bool *result) {
  if (env->exception) return js__error(env);

  auto &rt = env->rt();

  auto &local = js__value(object);

  if (!local.isObject()) {
    *result = false;

    return 0;
  }

  return js__call_into_javascript(env, [&] {
    *result = local.getObject(rt).instanceOf(rt, js__object(env, constructor).getFunction(rt));
  });
}

static inline bool
js__is_instance_of(js_env_t *env, const jsi::Value &value, const char *constructor) {
  auto &rt = env->rt();

  if (!value.isObject()) return false;

  auto global = rt.global().getProperty(rt, constructor);

  if (!global.isObject()) return false;

  return value.getObject(rt).instanceOf(rt, global.getObject(rt).getFunction(rt));
}

// Returns the built-in tag of an object, such as `Map Iterator` for an
// iterator over a map, as reported by `Object.prototype.toString()`.
static inline std::string
js__get_tag(js_env_t *env, const jsi::Value &value) {
  auto &rt = env->rt();

  if (!value.isObject()) return "";

  auto to_string = rt.global()
                     .getPropertyAsObject(rt, "Object")
                     .getPropertyAsObject(rt, "prototype")
                     .getPropertyAsFunction(rt, "toString");

  auto tag = to_string.callWithThis(rt, value.getObject(rt)).getString(rt).utf8(rt);

  return tag.substr(8, tag.size() - 9);
}

#define JS_PREDICATE(name, expression) \
  extern "C" int \
  name(js_env_t *env, js_value_t *value, bool *result) { \
    /* Allow continuing even with a pending exception */ \
    auto &rt = env->rt(); \
    auto &local = js__value(value); \
    (void) rt; \
    *result = (expression); \
    return 0; \
  }

JS_PREDICATE(js_is_undefined, local.isUndefined())
JS_PREDICATE(js_is_null, local.isNull())
JS_PREDICATE(js_is_boolean, local.isBool())
JS_PREDICATE(js_is_boolean_object, js__is_instance_of(env, local, "Boolean"))
JS_PREDICATE(js_is_number, local.isNumber())
JS_PREDICATE(js_is_number_object, js__is_instance_of(env, local, "Number"))
JS_PREDICATE(js_is_string, local.isString())
JS_PREDICATE(js_is_string_object, js__is_instance_of(env, local, "String"))
JS_PREDICATE(js_is_symbol, local.isSymbol())
JS_PREDICATE(js_is_symbol_object, js__is_instance_of(env, local, "Symbol"))
JS_PREDICATE(js_is_object, local.isObject())
JS_PREDICATE(js_is_function, local.isObject() && local.getObject(rt).isFunction(rt))
JS_PREDICATE(js_is_async_function, js__get_tag(env, local) == "AsyncFunction")
JS_PREDICATE(js_is_generator_function, js__get_tag(env, local) == "GeneratorFunction")
JS_PREDICATE(js_is_generator, js__get_tag(env, local) == "Generator")
JS_PREDICATE(js_is_arguments, js__get_tag(env, local) == "Arguments")
JS_PREDICATE(js_is_array, local.isObject() && local.getObject(rt).isArray(rt))
JS_PREDICATE(js_is_bigint, local.isBigInt())
JS_PREDICATE(js_is_bigint_object, js__is_instance_of(env, local, "BigInt"))
JS_PREDICATE(js_is_date, js__is_instance_of(env, local, "Date"))
JS_PREDICATE(js_is_regexp, js__is_instance_of(env, local, "RegExp"))
JS_PREDICATE(js_is_error, js__is_instance_of(env, local, "Error"))
JS_PREDICATE(js_is_promise, js__is_instance_of(env, local, "Promise"))
JS_PREDICATE(js_is_proxy, false)
JS_PREDICATE(js_is_map, js__is_instance_of(env, local, "Map"))
JS_PREDICATE(js_is_map_iterator, js__get_tag(env, local) == "Map Iterator")
JS_PREDICATE(js_is_set, js__is_instance_of(env, local, "Set"))
JS_PREDICATE(js_is_set_iterator, js__get_tag(env, local) == "Set Iterator")
JS_PREDICATE(js_is_weak_map, js__is_instance_of(env, local, "WeakMap"))
JS_PREDICATE(js_is_weak_set, js__is_instance_of(env, local, "WeakSet"))
JS_PREDICATE(js_is_weak_ref, js__is_instance_of(env, local, "WeakRef"))
JS_PREDICATE(js_is_arraybuffer, local.isObject() && local.getObject(rt).isArrayBuffer(rt))
JS_PREDICATE(js_is_detached_arraybuffer, local.isObject() && local.getObject(rt).isArrayBuffer(rt) && local.getObject(rt).getArrayBuffer(rt).detached(rt))
JS_PREDICATE(js_is_sharedarraybuffer, false)
JS_PREDICATE(js_is_typedarray, local.isObject() && local.getObject(rt).isTypedArray(rt))
JS_PREDICATE(js_is_int8array, js__get_tag(env, local) == "Int8Array")
JS_PREDICATE(js_is_uint8array, local.isObject() && local.getObject(rt).isUint8Array(rt))
JS_PREDICATE(js_is_uint8clampedarray, js__get_tag(env, local) == "Uint8ClampedArray")
JS_PREDICATE(js_is_int16array, js__get_tag(env, local) == "Int16Array")
JS_PREDICATE(js_is_uint16array, js__get_tag(env, local) == "Uint16Array")
JS_PREDICATE(js_is_int32array, js__get_tag(env, local) == "Int32Array")
JS_PREDICATE(js_is_uint32array, js__get_tag(env, local) == "Uint32Array")
JS_PREDICATE(js_is_float16array, false)
JS_PREDICATE(js_is_float32array, js__get_tag(env, local) == "Float32Array")
JS_PREDICATE(js_is_float64array, js__get_tag(env, local) == "Float64Array")
JS_PREDICATE(js_is_bigint64array, js__get_tag(env, local) == "BigInt64Array")
JS_PREDICATE(js_is_biguint64array, js__get_tag(env, local) == "BigUint64Array")
JS_PREDICATE(js_is_dataview, js__is_instance_of(env, local, "DataView"))
JS_PREDICATE(js_is_module_namespace, false)

#undef JS_PREDICATE

extern "C" int
js_is_int32(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  auto &local = js__value(value);

  if (local.isNumber()) {
    double integral;

    *result = modf(local.getNumber(), &integral) == 0.0 && integral >= INT32_MIN && integral <= INT32_MAX;
  } else {
    *result = false;
  }

  return 0;
}

extern "C" int
js_is_uint32(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  auto &local = js__value(value);

  if (local.isNumber()) {
    double integral;

    *result = modf(local.getNumber(), &integral) == 0.0 && integral >= 0.0 && integral <= UINT32_MAX;
  } else {
    *result = false;
  }

  return 0;
}

extern "C" int
js_is_external(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  auto &local = js__value(value);

  if (local.isObject()) {
    auto state = js__get_native_state(env, local.getObject(env->rt()));

    *result = state && state->external;
  } else {
    *result = false;
  }

  return 0;
}

extern "C" int
js_is_wrapped(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  auto &local = js__value(value);

  if (local.isObject()) {
    auto state = js__get_native_state(env, local.getObject(env->rt()));

    *result = state && state->wrap;
  } else {
    *result = false;
  }

  return 0;
}

extern "C" int
js_is_delegate(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

extern "C" int
js_get_object_type(js_env_t *env, js_value_t *value, js_object_type_t *result) {
  // Allow continuing even with a pending exception

  int err;

  bool is;

  // Classify by way of the individual predicates, in the order of precedence
  // documented for `js_object_type_t`, so that the two cannot drift apart.
#define V(type, predicate) \
  err = predicate(env, value, &is); \
  assert(err == 0); \
  if (is) { \
    *result = type; \
    return 0; \
  }

  V(js_array, js_is_array)
  V(js_arguments, js_is_arguments)
  V(js_date, js_is_date)
  V(js_regexp, js_is_regexp)
  V(js_error, js_is_error)
  V(js_promise, js_is_promise)
  V(js_proxy, js_is_proxy)
  V(js_generator, js_is_generator)
  V(js_map, js_is_map)
  V(js_set, js_is_set)
  V(js_map_iterator, js_is_map_iterator)
  V(js_set_iterator, js_is_set_iterator)
  V(js_weak_map, js_is_weak_map)
  V(js_weak_set, js_is_weak_set)
  V(js_weak_ref, js_is_weak_ref)
  V(js_arraybuffer, js_is_arraybuffer)
  V(js_sharedarraybuffer, js_is_sharedarraybuffer)
  V(js_typedarray, js_is_typedarray)
  V(js_dataview, js_is_dataview)
  V(js_module_namespace, js_is_module_namespace)
  V(js_boolean_object, js_is_boolean_object)
  V(js_number_object, js_is_number_object)
  V(js_string_object, js_is_string_object)
  V(js_symbol_object, js_is_symbol_object)
  V(js_bigint_object, js_is_bigint_object)
  V((js_object_type_t) js_external, js_is_external)
#undef V

  *result = (js_object_type_t) js_object;

  return 0;
}

extern "C" int
js_strict_equals(js_env_t *env, js_value_t *a, js_value_t *b, bool *result) {
  // Allow continuing even with a pending exception

  *result = jsi::Value::strictEquals(env->rt(), js__value(a), js__value(b));

  return 0;
}

extern "C" int
js_get_global(js_env_t *env, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach(env, env->rt().global());

  return 0;
}

extern "C" int
js_get_object_global(js_env_t *env, js_value_t *object, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach(env, env->rt().global());

  return 0;
}

extern "C" int
js_get_undefined(js_env_t *env, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach(env, jsi::Value::undefined());

  return 0;
}

extern "C" int
js_get_null(js_env_t *env, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach(env, jsi::Value::null());

  return 0;
}

extern "C" int
js_get_boolean(js_env_t *env, bool value, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach(env, jsi::Value(value));

  return 0;
}

extern "C" int
js_get_value_bool(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = js__value(value).getBool();

  return 0;
}

// Implements the ToUint32 conversion of ECMA-262 for a number.
static inline uint32_t
js__to_uint32(double number) {
  if (!std::isfinite(number)) return 0;

  double integral = std::trunc(number);

  double modulo = std::fmod(integral, 4294967296.0);

  if (modulo < 0) modulo += 4294967296.0;

  return static_cast<uint32_t>(modulo);
}

extern "C" int
js_get_value_int32(js_env_t *env, js_value_t *value, int32_t *result) {
  // Allow continuing even with a pending exception

  *result = static_cast<int32_t>(js__to_uint32(js__value(value).getNumber()));

  return 0;
}

extern "C" int
js_get_value_uint32(js_env_t *env, js_value_t *value, uint32_t *result) {
  // Allow continuing even with a pending exception

  *result = js__to_uint32(js__value(value).getNumber());

  return 0;
}

extern "C" int
js_get_value_int64(js_env_t *env, js_value_t *value, int64_t *result) {
  // Allow continuing even with a pending exception

  double number = js__value(value).getNumber();

  if (!std::isfinite(number)) *result = 0;
  else if (number <= static_cast<double>(INT64_MIN)) *result = INT64_MIN;
  else if (number >= static_cast<double>(INT64_MAX)) *result = INT64_MAX;
  else *result = static_cast<int64_t>(number);

  return 0;
}

extern "C" int
js_get_value_double(js_env_t *env, js_value_t *value, double *result) {
  // Allow continuing even with a pending exception

  *result = js__value(value).getNumber();

  return 0;
}

extern "C" int
js_get_value_bigint_int64(js_env_t *env, js_value_t *value, int64_t *result, bool *lossless) {
  // Allow continuing even with a pending exception

  auto &rt = env->rt();

  auto bigint = js__value(value).getBigInt(rt);

  *result = bigint.getInt64(rt);

  if (lossless) *lossless = bigint.isInt64(rt);

  return 0;
}

extern "C" int
js_get_value_bigint_uint64(js_env_t *env, js_value_t *value, uint64_t *result, bool *lossless) {
  // Allow continuing even with a pending exception

  auto &rt = env->rt();

  auto bigint = js__value(value).getBigInt(rt);

  *result = bigint.getUint64(rt);

  if (lossless) *lossless = bigint.isUint64(rt);

  return 0;
}

extern "C" int
js_get_value_bigint_words(js_env_t *env, js_value_t *value, int *sign, uint64_t *words, size_t len, size_t *result) {
  // Allow continuing even with a pending exception

  auto &rt = env->rt();

  auto hex = js__value(value).getBigInt(rt).toString(rt, 16).utf8(rt);

  bool negative = hex[0] == '-';

  if (negative) hex.erase(0, 1);

  if (hex == "0") hex.clear();

  size_t required = (hex.size() + 15) / 16;

  if (sign) *sign = negative;

  if (words) {
    size_t written = 0;

    for (size_t i = 0; i < required && i < len; i++) {
      size_t end = hex.size() - i * 16;
      size_t start = end > 16 ? end - 16 : 0;

      words[i] = strtoull(hex.substr(start, end - start).c_str(), NULL, 16);

      written++;
    }

    if (result) *result = written;
  } else if (result) {
    *result = required;
  }

  return 0;
}

static size_t
js__utf16le_prefix(size_t (*length)(const utf16_t *, size_t), const utf16_t *utf16, size_t utf16_len, size_t len) {
  size_t lo = 0, hi = utf16_len;

  while (lo < hi) {
    size_t mid = lo + (hi - lo + 1) / 2;

    if (length(utf16, mid) <= len) lo = mid;
    else hi = mid - 1;
  }

  return lo;
}

extern "C" int
js_get_value_string_utf8(js_env_t *env, js_value_t *value, utf8_t *str, size_t len, size_t *result) {
  // Allow continuing even with a pending exception

  auto string = js__value(value).getString(env->rt()).utf16(env->rt());

  auto utf16 = reinterpret_cast<const utf16_t *>(string.data());

  size_t utf16_len = string.size();

  if (str == NULL) {
    *result = utf8_length_from_utf16le(utf16, utf16_len);
  } else if (len != 0) {
    size_t prefix = utf16_len;

    if (utf8_length_from_utf16le(utf16, utf16_len) > len) {
      prefix = js__utf16le_prefix(utf8_length_from_utf16le, utf16, utf16_len, len);
    }

    size_t written = utf16le_convert_to_utf8(utf16, prefix, str);

    if (written < len) str[written] = '\0';

    if (result) *result = written;
  } else if (result) *result = 0;

  return 0;
}

extern "C" int
js_get_value_string_utf16le(js_env_t *env, js_value_t *value, utf16_t *str, size_t len, size_t *result) {
  // Allow continuing even with a pending exception

  auto string = js__value(value).getString(env->rt()).utf16(env->rt());

  size_t utf16_len = string.size();

  if (str == NULL) {
    *result = utf16_len;
  } else if (len != 0) {
    size_t written = len < utf16_len ? len : utf16_len;

    memcpy(str, string.data(), written * sizeof(utf16_t));

    if (written < len) str[written] = 0;

    if (result) *result = written;
  } else if (result) *result = 0;

  return 0;
}

extern "C" int
js_get_value_string_latin1(js_env_t *env, js_value_t *value, latin1_t *str, size_t len, size_t *result) {
  // Allow continuing even with a pending exception

  auto string = js__value(value).getString(env->rt()).utf16(env->rt());

  auto utf16 = reinterpret_cast<const utf16_t *>(string.data());

  size_t utf16_len = string.size();

  if (str == NULL) {
    *result = latin1_length_from_utf16le(utf16, utf16_len);
  } else if (len != 0) {
    size_t prefix = utf16_len;

    if (latin1_length_from_utf16le(utf16, utf16_len) > len) {
      prefix = js__utf16le_prefix(latin1_length_from_utf16le, utf16, utf16_len, len);
    }

    size_t written = utf16le_convert_to_latin1(utf16, prefix, str);

    if (written < len) str[written] = '\0';

    if (result) *result = written;
  } else if (result) *result = 0;

  return 0;
}

extern "C" int
js_get_value_external(js_env_t *env, js_value_t *value, void **result) {
  // Allow continuing even with a pending exception

  auto state = js__get_native_state(env, js__object(env, value));

  *result = state->external->data;

  return 0;
}

extern "C" int
js_get_value_date(js_env_t *env, js_value_t *value, double *result) {
  // Allow continuing even with a pending exception

  auto &rt = env->rt();

  auto get_time = rt.global()
                    .getPropertyAsObject(rt, "Date")
                    .getPropertyAsObject(rt, "prototype")
                    .getPropertyAsFunction(rt, "getTime");

  *result = get_time.callWithThis(rt, js__object(env, value)).getNumber();

  return 0;
}

extern "C" int
js_get_array_length(js_env_t *env, js_value_t *array, uint32_t *result) {
  // Allow continuing even with a pending exception

  auto &rt = env->rt();

  *result = static_cast<uint32_t>(js__object(env, array).getArray(rt).size(rt));

  return 0;
}

extern "C" int
js_get_array_elements(js_env_t *env, js_value_t *array, js_value_t **elements, size_t len, size_t offset, uint32_t *result) {
  if (env->exception) return js__error(env);

  auto &rt = env->rt();

  uint32_t written = 0;

  int err = js__call_into_javascript(env, [&] {
    auto local = js__object(env, array).getArray(rt);

    size_t m = local.size(rt);

    for (size_t i = 0, j = offset; i < len && j < m; i++, j++) {
      elements[i] = js__attach(env, local.getValueAtIndex(rt, j));

      written++;
    }
  });

  if (err < 0) return err;

  if (result) *result = written;

  return 0;
}

extern "C" int
js_set_array_elements(js_env_t *env, js_value_t *array, js_value_t *const elements[], size_t len, size_t offset) {
  if (env->exception) return js__error(env);

  auto &rt = env->rt();

  return js__call_into_javascript(env, [&] {
    auto local = js__object(env, array).getArray(rt);

    for (size_t i = 0, j = offset; i < len; i++, j++) {
      local.setValueAtIndex(rt, j, js__value(elements[i]));
    }
  });
}

extern "C" int
js_get_prototype(js_env_t *env, js_value_t *object, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__attach(env, js__object(env, object).getPrototype(env->rt()));

  return 0;
}

extern "C" int
js_set_prototype(js_env_t *env, js_value_t *object, js_value_t *prototype) {
  if (env->exception) return js__error(env);

  return js__call_into_javascript(env, [&] {
    js__object(env, object).setPrototype(env->rt(), js__value(prototype));
  });
}

static inline int
js__set_integrity_level(js_env_t *env, js_value_t *object, const char *method) {
  return js__call_into_javascript(env, [&] {
    js__builtin(env, "Object", method).call(env->rt(), js__value(object));
  });
}

extern "C" int
js_seal(js_env_t *env, js_value_t *object) {
  if (env->exception) return js__error(env);

  return js__set_integrity_level(env, object, "seal");
}

extern "C" int
js_freeze(js_env_t *env, js_value_t *object) {
  if (env->exception) return js__error(env);

  return js__set_integrity_level(env, object, "freeze");
}

extern "C" int
js_get_property_names(js_env_t *env, js_value_t *object, js_value_t **result) {
  if (env->exception) return js__error(env);

  std::optional<jsi::Value> names;

  int err = js__call_into_javascript(env, [&] {
    names.emplace(js__object(env, object).getPropertyNames(env->rt()));
  });

  if (err < 0) return err;

  if (result) *result = js__attach(env, std::move(*names));

  return 0;
}

// Determines whether a property key is a canonical array index, that is the
// string representation of an integer in the range [0, 2^32 - 2].
static inline bool
js__is_array_index(const std::string &key, uint32_t *index) {
  if (key.empty() || key.size() > 10) return false;

  if (key.size() > 1 && key[0] == '0') return false;

  uint64_t value = 0;

  for (char c : key) {
    if (c < '0' || c > '9') return false;

    value = value * 10 + (c - '0');
  }

  if (value >= UINT32_MAX) return false;

  *index = static_cast<uint32_t>(value);

  return true;
}

extern "C" int
js_get_filtered_property_names(js_env_t *env, js_value_t *object, js_key_collection_mode_t mode, js_property_filter_t property_filter, js_index_filter_t index_filter, js_key_conversion_mode_t key_conversion, js_value_t **result) {
  if (env->exception) return js__error(env);

  auto &rt = env->rt();

  std::optional<jsi::Value> names;

  int err = js__call_into_javascript(env, [&] {
    auto own_keys = js__builtin(env, "Reflect", "ownKeys");
    auto get_own_property_descriptor = js__builtin(env, "Object", "getOwnPropertyDescriptor");

    auto array = jsi::Array(rt, 0);

    std::vector<jsi::Value> seen;

    auto holder = js__copy(env, js__value(object));

    while (holder.isObject()) {
      auto keys = own_keys.call(rt, holder).getObject(rt).getArray(rt);

      for (size_t i = 0, n = keys.size(rt); i < n; i++) {
        auto key = keys.getValueAtIndex(rt, i);

        bool duplicate = false;

        for (auto &existing : seen) {
          if (jsi::Value::strictEquals(rt, existing, key)) {
            duplicate = true;
            break;
          }
        }

        if (duplicate) continue;

        seen.push_back(js__copy(env, key));

        if (key.isSymbol() ? (property_filter & js_property_skip_symbols) : (property_filter & js_property_skip_strings)) continue;

        auto descriptor = get_own_property_descriptor.call(rt, holder, key);

        if (!descriptor.isObject()) continue;

        auto local = descriptor.getObject(rt);

        if ((property_filter & js_property_only_writable) && !js__to_boolean(env, local.getProperty(rt, "writable"))) continue;
        if ((property_filter & js_property_only_enumerable) && !js__to_boolean(env, local.getProperty(rt, "enumerable"))) continue;
        if ((property_filter & js_property_only_configurable) && !js__to_boolean(env, local.getProperty(rt, "configurable"))) continue;

        uint32_t index;

        if (key.isString() && js__is_array_index(key.getString(rt).utf8(rt), &index)) {
          if (index_filter == js_index_skip_indices) continue;

          if (key_conversion == js_key_keep_numbers) key = jsi::Value(static_cast<double>(index));
        }

        array.push(rt, std::move(key));
      }

      if (mode == js_key_own_only) break;

      holder = holder.getObject(rt).getPrototype(rt);
    }

    names.emplace(std::move(array));
  });

  if (err < 0) return err;

  if (result) *result = js__attach(env, std::move(*names));

  return 0;
}

extern "C" int
js_get_property(js_env_t *env, js_value_t *object, js_value_t *key, js_value_t **result) {
  if (env->exception) return js__error(env);

  std::optional<jsi::Value> value;

  int err = js__call_into_javascript(env, [&] {
    value.emplace(js__object(env, object).getProperty(env->rt(), js__value(key)));
  });

  if (err < 0) return err;

  if (result) *result = js__attach(env, std::move(*value));

  return 0;
}

extern "C" int
js_has_property(js_env_t *env, js_value_t *object, js_value_t *key, bool *result) {
  if (env->exception) return js__error(env);

  bool value = false;

  int err = js__call_into_javascript(env, [&] {
    value = js__object(env, object).hasProperty(env->rt(), js__value(key));
  });

  if (err < 0) return err;

  if (result) *result = value;

  return 0;
}

extern "C" int
js_has_own_property(js_env_t *env, js_value_t *object, js_value_t *key, bool *result) {
  if (env->exception) return js__error(env);

  auto &rt = env->rt();

  bool value = false;

  int err = js__call_into_javascript(env, [&] {
    value = js__builtin(env, "Object", "hasOwn").call(rt, js__value(object), js__value(key)).getBool();
  });

  if (err < 0) return err;

  if (result) *result = value;

  return 0;
}

extern "C" int
js_set_property(js_env_t *env, js_value_t *object, js_value_t *key, js_value_t *value) {
  if (env->exception) return js__error(env);

  return js__call_into_javascript(env, [&] {
    js__object(env, object).setProperty(env->rt(), js__value(key), js__value(value));
  });
}

extern "C" int
js_delete_property(js_env_t *env, js_value_t *object, js_value_t *key, bool *result) {
  if (env->exception) return js__error(env);

  auto &rt = env->rt();

  bool value = false;

  int err = js__call_into_javascript(env, [&] {
    value = js__builtin(env, "Reflect", "deleteProperty").call(rt, js__value(object), js__value(key)).getBool();
  });

  if (err < 0) return err;

  if (result) *result = value;

  return 0;
}

extern "C" int
js_get_named_property(js_env_t *env, js_value_t *object, const char *name, js_value_t **result) {
  if (env->exception) return js__error(env);

  int err;

  js_value_t *key;
  err = js_create_string_utf8(env, reinterpret_cast<const utf8_t *>(name), -1, &key);
  if (err < 0) return err;

  return js_get_property(env, object, key, result);
}

extern "C" int
js_has_named_property(js_env_t *env, js_value_t *object, const char *name, bool *result) {
  if (env->exception) return js__error(env);

  int err;

  js_value_t *key;
  err = js_create_string_utf8(env, reinterpret_cast<const utf8_t *>(name), -1, &key);
  if (err < 0) return err;

  return js_has_property(env, object, key, result);
}

extern "C" int
js_set_named_property(js_env_t *env, js_value_t *object, const char *name, js_value_t *value) {
  if (env->exception) return js__error(env);

  int err;

  js_value_t *key;
  err = js_create_string_utf8(env, reinterpret_cast<const utf8_t *>(name), -1, &key);
  if (err < 0) return err;

  return js_set_property(env, object, key, value);
}

extern "C" int
js_delete_named_property(js_env_t *env, js_value_t *object, const char *name, bool *result) {
  if (env->exception) return js__error(env);

  int err;

  js_value_t *key;
  err = js_create_string_utf8(env, reinterpret_cast<const utf8_t *>(name), -1, &key);
  if (err < 0) return err;

  return js_delete_property(env, object, key, result);
}

extern "C" int
js_get_element(js_env_t *env, js_value_t *object, uint32_t index, js_value_t **result) {
  if (env->exception) return js__error(env);

  int err;

  js_value_t *key;
  err = js_create_uint32(env, index, &key);
  assert(err == 0);

  return js_get_property(env, object, key, result);
}

extern "C" int
js_has_element(js_env_t *env, js_value_t *object, uint32_t index, bool *result) {
  if (env->exception) return js__error(env);

  int err;

  js_value_t *key;
  err = js_create_uint32(env, index, &key);
  assert(err == 0);

  return js_has_property(env, object, key, result);
}

extern "C" int
js_set_element(js_env_t *env, js_value_t *object, uint32_t index, js_value_t *value) {
  if (env->exception) return js__error(env);

  int err;

  js_value_t *key;
  err = js_create_uint32(env, index, &key);
  assert(err == 0);

  return js_set_property(env, object, key, value);
}

extern "C" int
js_delete_element(js_env_t *env, js_value_t *object, uint32_t index, bool *result) {
  if (env->exception) return js__error(env);

  int err;

  js_value_t *key;
  err = js_create_uint32(env, index, &key);
  assert(err == 0);

  return js_delete_property(env, object, key, result);
}

extern "C" int
js_get_string_view(js_env_t *env, js_value_t *string, js_string_encoding_t *encoding, const void **str, size_t *len, js_string_view_t **result) {
  // Allow continuing even with a pending exception

  auto view = new js_string_view_t();

  view->utf16 = js__value(string).getString(env->rt()).utf16(env->rt());

  bool latin1 = true;

  for (auto c : view->utf16) {
    if (c > 0xff) {
      latin1 = false;
      break;
    }
  }

  if (latin1) {
    view->encoding = js_latin1;
    view->latin1 = std::string(view->utf16.begin(), view->utf16.end());
    view->utf16.clear();

    if (str) *str = view->latin1.data();

    if (len) *len = view->latin1.size();
  } else {
    view->encoding = js_utf16le;

    if (str) *str = view->utf16.data();

    if (len) *len = view->utf16.size();
  }

  if (encoding) *encoding = view->encoding;

  *result = view;

  return 0;
}

extern "C" int
js_release_string_view(js_env_t *env, js_string_view_t *view) {
  // Allow continuing even with a pending exception

  delete view;

  return 0;
}

extern "C" int
js_get_callback_info(js_env_t *env, const js_callback_info_t *info, size_t *argc, js_value_t *argv[], js_value_t **receiver, void **data) {
  // Allow continuing even with a pending exception

  if (argv) {
    size_t i = 0, n = info->argc < *argc ? info->argc : *argc;

    for (; i < n; i++) {
      argv[i] = js__handle(&info->argv[i]);
    }

    n = *argc;

    for (; i < n; i++) {
      argv[i] = js__handle(&js__undefined);
    }
  }

  if (argc) {
    *argc = info->argc;
  }

  if (receiver) {
    *receiver = js__handle(info->receiver);
  }

  if (data) {
    *data = info->callback->data;
  }

  return 0;
}

extern "C" int
js_get_typed_callback_info(const js_typed_callback_info_t *info, js_env_t **env, void **data) {
  // Allow continuing even with a pending exception

  return 0;
}

// Host functions can't be called as constructors in Hermes, so there's never
// a new target to report.
extern "C" int
js_get_new_target(js_env_t *env, const js_callback_info_t *info, js_value_t **result) {
  // Allow continuing even with a pending exception

  *result = js__handle(&js__undefined);

  return 0;
}

extern "C" int
js_get_arraybuffer_info(js_env_t *env, js_value_t *arraybuffer, void **data, size_t *len) {
  // Allow continuing even with a pending exception

  auto &rt = env->rt();

  auto local = js__object(env, arraybuffer).getArrayBuffer(rt);

  if (data) *data = local.data(rt);

  if (len) *len = local.size(rt);

  return 0;
}

extern "C" int
js_get_sharedarraybuffer_info(js_env_t *env, js_value_t *sharedarraybuffer, void **data, size_t *len) {
  return js__unsupported(env);
}

extern "C" int
js_get_typedarray_info(js_env_t *env, js_value_t *typedarray, js_typedarray_type_t *type, void **data, size_t *len, js_value_t **arraybuffer, size_t *offset) {
  // Allow continuing even with a pending exception

  auto &rt = env->rt();

  auto local = js__object(env, typedarray).getTypedArray(rt);

  size_t byte_offset = local.byteOffset(rt);

  if (type) {
    auto tag = js__get_tag(env, js__value(typedarray));

    for (auto candidate : js__typedarray_types) {
      if (tag == js__typedarray_name(candidate)) {
        *type = candidate;
        break;
      }
    }
  }

  if (data || arraybuffer) {
    auto buffer = local.buffer(rt);

    if (data) *data = buffer.data(rt) + byte_offset;

    if (arraybuffer) *arraybuffer = js__attach(env, std::move(buffer));
  }

  if (len) *len = local.length(rt);

  if (offset) *offset = byte_offset;

  return 0;
}

extern "C" int
js_get_dataview_info(js_env_t *env, js_value_t *dataview, void **data, size_t *len, js_value_t **arraybuffer, size_t *offset) {
  // Allow continuing even with a pending exception

  auto &rt = env->rt();

  auto local = js__object(env, dataview);

  auto byte_offset = static_cast<size_t>(local.getProperty(rt, "byteOffset").getNumber());

  if (data || arraybuffer) {
    auto buffer = local.getPropertyAsObject(rt, "buffer").getArrayBuffer(rt);

    if (data) *data = buffer.data(rt) + byte_offset;

    if (arraybuffer) *arraybuffer = js__attach(env, std::move(buffer));
  }

  if (len) *len = static_cast<size_t>(local.getProperty(rt, "byteLength").getNumber());

  if (offset) *offset = byte_offset;

  return 0;
}

static inline jsi::Value
js__call_function(js_env_t *env, const jsi::Value &receiver, js_value_t *function, size_t argc, js_value_t *const argv[]) {
  auto &rt = env->rt();

  auto local = js__object(env, function).getFunction(rt);

  auto args = js__copy_values(env, argv, argc);

  if (receiver.isObject()) {
    return local.callWithThis(rt, receiver.getObject(rt), static_cast<const jsi::Value *>(args.data()), args.size());
  }

  if (receiver.isUndefined() || receiver.isNull()) {
    return local.call(rt, static_cast<const jsi::Value *>(args.data()), args.size());
  }

  // JSI only passes object receivers, so primitive receivers go through
  // `Reflect.apply()` to reach strict mode functions unboxed.
  auto array = jsi::Array(rt, args.size());

  for (size_t i = 0; i < args.size(); i++) array.setValueAtIndex(rt, i, args[i]);

  return js__builtin(env, "Reflect", "apply").call(rt, local, receiver, array);
}

static inline int
js__call(js_env_t *env, js_value_t *receiver, js_value_t *function, size_t argc, js_value_t *const argv[], js_value_t **result, bool always_checkpoint) {
  std::optional<jsi::Value> value;

  int err = js__call_into_javascript(
    env,
    [&] {
      value.emplace(js__call_function(env, js__value(receiver), function, argc, argv));
    },
    always_checkpoint
  );

  if (err < 0) return err;

  if (result) *result = js__attach(env, std::move(*value));

  return 0;
}

extern "C" int
js_call_function(js_env_t *env, js_value_t *receiver, js_value_t *function, size_t argc, js_value_t *const argv[], js_value_t **result) {
  if (env->exception) return js__error(env);

  return js__call(env, receiver, function, argc, argv, result, false);
}

extern "C" int
js_call_function_with_checkpoint(js_env_t *env, js_value_t *receiver, js_value_t *function, size_t argc, js_value_t *const argv[], js_value_t **result) {
  if (env->exception) return js__error(env);

  return js__call(env, receiver, function, argc, argv, result, true);
}

extern "C" int
js_queue_microtask(js_env_t *env, js_value_t *function) {
  // Allow continuing even with a pending exception

  env->rt().queueMicrotask(js__object(env, function).getFunction(env->rt()));

  return 0;
}

extern "C" int
js_queue_microtask_with_callback(js_env_t *env, js_task_cb cb, void *data) {
  // Allow continuing even with a pending exception

  auto &rt = env->rt();

  auto function = jsi::Function::createFromHostFunction(
    rt,
    jsi::PropNameID::forAscii(rt, "microtask"),
    0,
    [env, cb, data](jsi::Runtime &rt, const jsi::Value &receiver, const jsi::Value *argv, size_t argc) {
      int err;

      js_handle_scope_t *scope;
      err = js_open_handle_scope(env, &scope);
      assert(err == 0);

      cb(env, data);

      err = js_close_handle_scope(env, scope);
      assert(err == 0);

      if (env->exception) {
        auto error = std::move(*env->exception);

        env->exception.reset();

        throw jsi::JSError(std::move(error), "", "");
      }

      return jsi::Value::undefined();
    }
  );

  rt.queueMicrotask(function);

  return 0;
}

extern "C" int
js_new_instance(js_env_t *env, js_value_t *constructor, size_t argc, js_value_t *const argv[], js_value_t **result) {
  if (env->exception) return js__error(env);

  auto &rt = env->rt();

  std::optional<jsi::Value> value;

  int err = js__call_into_javascript(env, [&] {
    auto args = js__copy_values(env, argv, argc);

    value.emplace(js__object(env, constructor).getFunction(rt).callAsConstructor(rt, static_cast<const jsi::Value *>(args.data()), args.size()));
  });

  if (err < 0) return err;

  if (result) *result = js__attach(env, std::move(*value));

  return 0;
}

static inline bool
js__threadsafe_queue_push(js_threadsafe_queue_t *queue, void *data) {
  uv_mutex_lock(&queue->lock);

  if (queue->closed) {
    uv_mutex_unlock(&queue->lock);

    return false;
  }

  if (queue->len == queue->capacity) {
    size_t capacity = queue->capacity ? queue->capacity << 1u : 16;

    auto resized = static_cast<void **>(realloc(queue->queue, capacity * sizeof(void *)));

    if (queue->head > 0) {
      size_t head_len = queue->capacity - queue->head;

      if (head_len > 0) {
        memmove(&resized[capacity - head_len], &resized[queue->head], head_len * sizeof(void *));
      }

      queue->head = capacity - head_len;
    }

    queue->queue = resized;
    queue->capacity = capacity;
  }

  queue->queue[queue->tail] = data;
  queue->tail = (queue->tail + 1) & (queue->capacity - 1);
  queue->len++;

  uv_mutex_unlock(&queue->lock);

  return true;
}

static inline bool
js__threadsafe_queue_pop(js_threadsafe_queue_t *queue, void **result) {
  uv_mutex_lock(&queue->lock);

  if (queue->len == 0) {
    uv_mutex_unlock(&queue->lock);

    return false;
  }

  void *data = queue->queue[queue->head];

  queue->head = (queue->head + 1) & (queue->capacity - 1);
  queue->len--;

  if (queue->capacity > 16 && queue->len <= queue->capacity / 4) {
    size_t capacity = queue->capacity >> 1u;

    auto resized = static_cast<void **>(malloc(capacity * sizeof(void *)));

    for (size_t i = 0; i < queue->len; i++) {
      resized[i] = queue->queue[(queue->head + i) & (queue->capacity - 1)];
    }

    free(queue->queue);

    queue->queue = resized;
    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = queue->len;
  }

  uv_mutex_unlock(&queue->lock);

  *result = data;

  return true;
}

static inline void
js__threadsafe_queue_close(js_threadsafe_queue_t *queue) {
  uv_mutex_lock(&queue->lock);

  queue->closed = true;

  uv_mutex_unlock(&queue->lock);
}

static inline void
js__threadsafe_function_finalize(js_threadsafe_function_t *function) {
  auto env = function->env;

  if (function->finalize_cb) {
    function->finalize_cb(env, function->context, function->finalize_hint);
  }

  intrusive_list_remove(&env->threadsafe_functions, &function->list);

  uv_mutex_destroy(&function->queue.lock);

  free(function->queue.queue);

  delete function;
}

static void
js__on_threadsafe_function_close(uv_handle_t *handle) {
  auto function = static_cast<js_threadsafe_function_t *>(handle->data);

  js__threadsafe_function_finalize(function);
}

static inline void
js__threadsafe_function_close(js_threadsafe_function_t *function) {
  uv_close(reinterpret_cast<uv_handle_t *>(&function->async), js__on_threadsafe_function_close);
}

static inline bool
js__threadsafe_function_call(js_threadsafe_function_t *function) {
  int err;

  auto env = function->env;

  void *data;

  if (js__threadsafe_queue_pop(&function->queue, &data)) {
    js_handle_scope_t *scope;
    err = js_open_handle_scope(env, &scope);
    assert(err == 0);

    auto callback = function->function ? js__attach(env, js__copy(env, *function->function)) : NULL;

    if (function->cb) {
      function->cb(env, callback, function->context, data);
    } else if (callback) {
      js_value_t *receiver;
      err = js_get_undefined(env, &receiver);
      assert(err == 0);

      err = js_call_function(env, receiver, callback, 0, NULL, NULL);
      (void) err;
    }

    err = js_close_handle_scope(env, scope);
    assert(err == 0);

    return true;
  }

  if (function->thread_count.load(std::memory_order_relaxed) == 0) {
    js__threadsafe_function_close(function);
  }

  return false;
}

static inline void
js__threadsafe_function_signal(js_threadsafe_function_t *function) {
  int err;

  int state = function->state.fetch_or(js_threadsafe_function_pending, std::memory_order_acq_rel);

  if (state & js_threadsafe_function_running) {
    return;
  }

  err = uv_async_send(&function->async);
  assert(err == 0);
}

static inline void
js__threadsafe_function_dispatch(js_threadsafe_function_t *function) {
  bool done = false;

  int iterations = 1024;

  while (!done && --iterations >= 0) {
    function->state.store(js_threadsafe_function_running, std::memory_order_release);

    done = js__threadsafe_function_call(function) == false;

    if (function->state.exchange(js_threadsafe_function_idle, std::memory_order_acq_rel) != js_threadsafe_function_running) {
      done = false;
    }
  }

  if (!done) js__threadsafe_function_signal(function);
}

static void
js__on_threadsafe_function_async(uv_async_t *handle) {
  auto function = static_cast<js_threadsafe_function_t *>(handle->data);

  js__threadsafe_function_dispatch(function);
}

extern "C" int
js_create_threadsafe_function(js_env_t *env, js_value_t *function, size_t queue_limit, size_t initial_thread_count, js_finalize_cb finalize_cb, void *finalize_hint, void *context, js_threadsafe_function_cb cb, js_threadsafe_function_t **result) {
  if (env->exception) return js__error(env);

  int err;

  if (function == NULL && cb == NULL) {
    err = js_throw_error(env, NULL, "Either a function or a callback must be provided");
    assert(err == 0);

    return js__error(env);
  }

  if (initial_thread_count == 0) {
    err = js_throw_error(env, NULL, "Initial thread count must be greater than 0");
    assert(err == 0);

    return js__error(env);
  }

  auto threadsafe_function = new js_threadsafe_function_t();

  threadsafe_function->env = env;
  threadsafe_function->context = context;
  threadsafe_function->finalize_cb = finalize_cb;
  threadsafe_function->finalize_hint = finalize_hint;
  threadsafe_function->cb = cb;

  threadsafe_function->state = js_threadsafe_function_idle;
  threadsafe_function->thread_count = static_cast<int>(initial_thread_count);

  if (function) threadsafe_function->function.emplace(js__copy(env, js__value(function)));

  err = uv_async_init(env->loop, &threadsafe_function->async, js__on_threadsafe_function_async);
  assert(err == 0);

  threadsafe_function->async.data = threadsafe_function;

  auto queue = &threadsafe_function->queue;

  queue->queue = NULL;
  queue->len = 0;
  queue->capacity = 0;
  queue->head = 0;
  queue->tail = 0;
  queue->closed = false;

  err = uv_mutex_init(&queue->lock);
  assert(err == 0);

  intrusive_list_prepend(&env->threadsafe_functions, &threadsafe_function->list);

  *result = threadsafe_function;

  return 0;
}

extern "C" int
js_get_threadsafe_function_context(js_threadsafe_function_t *function, void **result) {
  // Allow continuing even with a pending exception

  *result = function->context;

  return 0;
}

extern "C" int
js_call_threadsafe_function(js_threadsafe_function_t *function, void *data, js_threadsafe_function_call_mode_t mode) {
  if (function->thread_count.load(std::memory_order_relaxed) == 0) return -1;

  if (js__threadsafe_queue_push(&function->queue, data)) {
    js__threadsafe_function_signal(function);

    return 0;
  }

  return -1;
}

extern "C" int
js_acquire_threadsafe_function(js_threadsafe_function_t *function) {
  int thread_count = function->thread_count.load(std::memory_order_relaxed);

  while (thread_count != 0) {
    if (function->thread_count.compare_exchange_weak(thread_count, thread_count + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
      return 0;
    }
  }

  return -1;
}

extern "C" int
js_release_threadsafe_function(js_threadsafe_function_t *function, js_threadsafe_function_release_mode_t mode) {
  int thread_count = function->thread_count.load(std::memory_order_relaxed);

  bool abort = mode == js_threadsafe_function_abort;

  while (thread_count != 0) {
    if (function->thread_count.compare_exchange_weak(thread_count, abort ? 0 : thread_count - 1, std::memory_order_acquire, std::memory_order_relaxed)) {
      if (abort || thread_count == 1) {
        js__threadsafe_queue_close(&function->queue);

        js__threadsafe_function_signal(function);
      }

      return 0;
    }
  }

  return -1;
}

extern "C" int
js_ref_threadsafe_function(js_env_t *env, js_threadsafe_function_t *function) {
  // Allow continuing even with a pending exception

  uv_ref(reinterpret_cast<uv_handle_t *>(&function->async));

  return 0;
}

extern "C" int
js_unref_threadsafe_function(js_env_t *env, js_threadsafe_function_t *function) {
  // Allow continuing even with a pending exception

  uv_unref(reinterpret_cast<uv_handle_t *>(&function->async));

  return 0;
}

extern "C" int
js_add_teardown_callback(js_env_t *env, js_teardown_cb callback, void *data) {
  if (env->exception) return js__error(env);

  auto task = new js_teardown_task_t();

  task->type = js_teardown_task_t::js_immediate_teardown;
  task->immediate.cb = callback;
  task->data = data;

  intrusive_list_prepend(&env->teardown_queue.tasks, &task->list);

  return 0;
}

extern "C" int
js_remove_teardown_callback(js_env_t *env, js_teardown_cb callback, void *data) {
  if (env->exception) return js__error(env);

  if (env->destroying) return 0;

  intrusive_list_for_each(next, &env->teardown_queue.tasks) {
    auto task = intrusive_entry(next, js_teardown_task_t, list);

    if (task->type == js_teardown_task_t::js_immediate_teardown && task->immediate.cb == callback && task->data == data) {
      intrusive_list_remove(&env->teardown_queue.tasks, &task->list);

      delete task;

      return 0;
    }
  }

  return 0;
}

extern "C" int
js_add_deferred_teardown_callback(js_env_t *env, js_deferred_teardown_cb callback, void *data, js_deferred_teardown_t **result) {
  if (env->exception) return js__error(env);

  auto task = new js_teardown_task_t();

  task->type = js_teardown_task_t::js_deferred_teardown;
  task->deferred.cb = callback;
  task->deferred.handle.env = env;
  task->data = data;

  intrusive_list_prepend(&env->teardown_queue.tasks, &task->list);

  env->refs++;

  if (result) *result = &task->deferred.handle;

  return 0;
}

extern "C" int
js_finish_deferred_teardown_callback(js_deferred_teardown_t *handle) {
  // Allow continuing even with a pending exception

  int err;

  auto env = handle->env;

  intrusive_list_for_each(next, &env->teardown_queue.tasks) {
    auto task = intrusive_entry(next, js_teardown_task_t, list);

    if (task->type == js_teardown_task_t::js_deferred_teardown && &task->deferred.handle == handle) {
      intrusive_list_remove(&env->teardown_queue.tasks, &task->list);

      if (--env->refs == 0 && env->destroying) {
        err = uv_async_send(&env->teardown);
        assert(err == 0);
      }

      delete task;

      return 0;
    }
  }

  return -1;
}

extern "C" int
js_throw(js_env_t *env, js_value_t *error) {
  if (env->exception) return js__error(env);

  env->exception.emplace(js__copy(env, js__value(error)));

  return 0;
}

static inline int
js__throw(js_env_t *env, const char *type, const char *code, const char *message) {
  if (env->exception) return js__error(env);

  env->exception.emplace(js__create_error(env, type, code, js__string(env, message)));

  js__propagate_exception(env);

  return 0;
}

static inline int
js__vthrowf(js_env_t *env, const char *type, const char *code, const char *message, va_list args) {
  if (env->exception) return js__error(env);

  va_list args_copy;
  va_copy(args_copy, args);

  int size = vsnprintf(NULL, 0, message, args_copy);

  va_end(args_copy);

  if (size < 0) return size;

  auto formatted = std::string(size, '\0');

  vsnprintf(formatted.data(), size + 1, message, args);

  return js__throw(env, type, code, formatted.c_str());
}

#define JS_THROW(name, type) \
  extern "C" int \
  js_throw_##name(js_env_t *env, const char *code, const char *message) { \
    return js__throw(env, type, code, message); \
  } \
\
  extern "C" int \
  js_throw_##name##f(js_env_t *env, const char *code, const char *message, ...) { \
    va_list args; \
    va_start(args, message); \
    int err = js__vthrowf(env, type, code, message, args); \
    va_end(args); \
    return err; \
  }

#define JS_VTHROW(name, type) \
  extern "C" int \
  js_throw_##name(js_env_t *env, const char *code, const char *message, va_list args) { \
    return js__vthrowf(env, type, code, message, args); \
  }

JS_THROW(error, "Error")
JS_THROW(type_error, "TypeError")
JS_THROW(range_error, "RangeError")
JS_THROW(syntax_error, "SyntaxError")
JS_THROW(reference_error, "ReferenceError")

JS_VTHROW(verrorf, "Error")
JS_VTHROW(type_verrorf, "TypeError")
JS_VTHROW(range_verrorf, "RangeError")
JS_VTHROW(syntax_verrorf, "SyntaxError")
JS_VTHROW(reference_verrorf, "ReferenceError")

#undef JS_THROW
#undef JS_VTHROW

extern "C" int
js_is_exception_pending(js_env_t *env, bool *result) {
  // Allow continuing even with a pending exception

  *result = env->exception.has_value();

  return 0;
}

extern "C" int
js_get_and_clear_last_exception(js_env_t *env, js_value_t **result) {
  // Allow continuing even with a pending exception

  if (!env->exception) return js_get_undefined(env, result);

  *result = js__attach(env, std::move(*env->exception));

  env->exception.reset();

  return 0;
}

extern "C" int
js_fatal_exception(js_env_t *env, js_value_t *error) {
  // Allow continuing even with a pending exception

  js__on_uncaught_exception(env, js__copy(env, js__value(error)));

  return 0;
}

extern "C" int
js_terminate_execution(js_env_t *env) {
  // Allow continuing even with a pending exception

  env->terminated = true;

  env->runtime->asyncTriggerTimeout();

  return 0;
}

extern "C" int
js_adjust_external_memory(js_env_t *env, int64_t change_in_bytes, int64_t *result) {
  // Allow continuing even with a pending exception

  env->external_memory += change_in_bytes;

  if (result) *result = env->external_memory;

  return 0;
}

extern "C" int
js_request_garbage_collection(js_env_t *env) {
  // Allow continuing even with a pending exception

  if (env->platform->options.expose_garbage_collection) {
    env->rt().instrumentation().collectGarbage("js_request_garbage_collection");
  }

  return 0;
}

// Hermes reports collections through a callback fixed at runtime creation that
// may be invoked from its background collector thread.
extern "C" int
js_enable_garbage_collection_tracking(js_env_t *env, const js_garbage_collection_tracking_options_t *options, void *data, js_garbage_collection_tracking_t **result) {
  return js__unsupported(env);
}

extern "C" int
js_disable_garbage_collection_tracking(js_env_t *env, js_garbage_collection_tracking_t *tracking) {
  return js__unsupported(env);
}

extern "C" int
js_get_heap_statistics(js_env_t *env, js_heap_statistics_t *result) {
  // Allow continuing even with a pending exception

  auto info = env->rt().instrumentation().getHeapInfo(false);

  result->total_heap_size = static_cast<size_t>(info["hermes_heapSize"]);
  result->used_heap_size = static_cast<size_t>(info["hermes_allocatedBytes"]);

  if (result->version >= 1) {
    result->external_memory = static_cast<size_t>(info["hermes_externalBytes"] + env->external_memory);
  }

  return 0;
}

extern "C" int
js_get_heap_space_statistics(js_env_t *env, js_heap_space_statistics_t statistics[], size_t len, size_t offset, size_t *result) {
  if (result) *result = 0;

  return 0;
}

extern "C" int
js_create_inspector(js_env_t *env, js_inspector_t **result) {
  return js__unsupported(env);
}

extern "C" int
js_destroy_inspector(js_env_t *env, js_inspector_t *inspector) {
  return js__unsupported(env);
}

extern "C" int
js_on_inspector_response(js_env_t *env, js_inspector_t *inspector, js_inspector_message_cb cb, void *data) {
  return 0;
}

extern "C" int
js_on_inspector_paused(js_env_t *env, js_inspector_t *inspector, js_inspector_paused_cb cb, void *data) {
  return 0;
}

extern "C" int
js_connect_inspector(js_env_t *env, js_inspector_t *inspector) {
  return js__unsupported(env);
}

extern "C" int
js_send_inspector_request(js_env_t *env, js_inspector_t *inspector, const char *message, size_t len) {
  return js__unsupported(env);
}

extern "C" int
js_attach_context_to_inspector(js_env_t *env, js_inspector_t *inspector, js_context_t *context, const char *name, size_t len) {
  return js__unsupported(env);
}

extern "C" int
js_detach_context_from_inspector(js_env_t *env, js_inspector_t *inspector, js_context_t *context) {
  return js__unsupported(env);
}
