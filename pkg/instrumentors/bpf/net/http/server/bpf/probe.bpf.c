#include "arguments.h"
#include "span_context.h"
#include "go_context.h"
#include "go_types.h"

char __license[] SEC("license") = "Dual MIT/GPL";

#define MAX_SIZE 100
#define MAX_CONCURRENT 50
#define W3C_KEY_LENGTH 11
#define W3C_VAL_LENGTH 55

struct http_request_t {
    u64 start_time;
    u64 end_time;
    char method[MAX_SIZE];
    char path[MAX_SIZE];
    struct span_context sc;
    struct span_context psc;
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, void*);
	__type(value, struct http_request_t);
	__uint(max_entries, MAX_CONCURRENT);
} context_to_http_events SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
} events SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(struct map_bucket));
	__uint(max_entries, 1);
} golang_mapbucket_storage_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(struct span_context));
	__uint(max_entries, 1);
} parent_span_context_storage_map SEC(".maps");

// Injected in init
volatile const u64 method_ptr_pos;
volatile const u64 url_ptr_pos;
volatile const u64 path_ptr_pos;
volatile const u64 ctx_ptr_pos;
volatile const u64 headers_ptr_pos;

static __always_inline struct span_context* extract_context_from_req_headers(void* headers_ptr_ptr) {
    void* headers_ptr;
    bpf_probe_read(&headers_ptr, sizeof(headers_ptr), headers_ptr_ptr);
    u64 headers_count = 0;
    bpf_probe_read(&headers_count, sizeof(headers_count), headers_ptr);
    if (headers_count == 0) {
        return NULL;
    }
    unsigned char log_2_bucket_count;
    bpf_probe_read(&log_2_bucket_count, sizeof(log_2_bucket_count), headers_ptr + 9);
    u64 bucket_count = 1 << log_2_bucket_count;
    void* header_buckets;
    bpf_probe_read(&header_buckets, sizeof(header_buckets), headers_ptr + 16);
    u32 map_id = 0;
    struct map_bucket* map_value = bpf_map_lookup_elem(&golang_mapbucket_storage_map, &map_id);
    if (!map_value) {
        return NULL;
    }
    // Currently iterating only over the first bucket
    if (bucket_count >= 2) {
        bucket_count = 1;
    }
    for (u64 j=0; j<bucket_count; j++) {
        bpf_probe_read(map_value, sizeof(struct map_bucket), header_buckets + (j * sizeof(struct map_bucket)));
        for (u64 i=0; i<8; i++) {
            if (map_value->tophash[i] == 0) {
                continue;
            }
            if (map_value->keys[i].len != W3C_KEY_LENGTH) {
                continue;
            }
            char current_header_key[W3C_KEY_LENGTH];
            bpf_probe_read(current_header_key, sizeof(current_header_key), map_value->keys[i].str);
            if (!bpf_memcmp(current_header_key, "traceparent", W3C_KEY_LENGTH) && !bpf_memcmp(current_header_key, "Traceparent", W3C_KEY_LENGTH)) {
               continue;
            }
            void* traceparent_header_value_ptr = map_value->values[i].array;
            struct go_string traceparent_header_value_go_str;
            bpf_probe_read(&traceparent_header_value_go_str, sizeof(traceparent_header_value_go_str), traceparent_header_value_ptr);
            if (traceparent_header_value_go_str.len != W3C_VAL_LENGTH) {
                continue;
            }
            char traceparent_header_value[W3C_VAL_LENGTH];
            bpf_probe_read(&traceparent_header_value, sizeof(traceparent_header_value), traceparent_header_value_go_str.str);
            struct span_context* parent_span_context = bpf_map_lookup_elem(&parent_span_context_storage_map, &map_id);
            if (!parent_span_context) {
                return NULL;
            }
            w3c_string_to_span_context(traceparent_header_value, parent_span_context);
            return parent_span_context;
        }
    }
    return NULL;
}

// This instrumentation attaches uprobe to the following function:
// func (mux *ServeMux) ServeHTTP(w ResponseWriter, r *Request)
SEC("uprobe/ServerMux_ServeHTTP")
int uprobe_ServerMux_ServeHTTP(struct pt_regs *ctx) {
    u64 request_pos = 4;
    struct http_request_t httpReq = {};
    httpReq.start_time = bpf_ktime_get_ns();

    // Get request struct
    void* req_ptr = get_argument(ctx, request_pos);

    // Get method from request
    void* method_ptr = 0;
    bpf_probe_read(&method_ptr, sizeof(method_ptr), (void *)(req_ptr+method_ptr_pos));
    u64 method_len = 0;
    bpf_probe_read(&method_len, sizeof(method_len), (void *)(req_ptr+(method_ptr_pos+8)));
    u64 method_size = sizeof(httpReq.method);
    method_size = method_size < method_len ? method_size : method_len;
    bpf_probe_read(&httpReq.method, method_size, method_ptr);

    // get path from Request.URL
    void *url_ptr = 0;
    bpf_probe_read(&url_ptr, sizeof(url_ptr), (void *)(req_ptr+url_ptr_pos));
    void* path_ptr = 0;
    bpf_probe_read(&path_ptr, sizeof(path_ptr), (void *)(url_ptr+path_ptr_pos));
    u64 path_len = 0;
    bpf_probe_read(&path_len, sizeof(path_len), (void *)(url_ptr+(path_ptr_pos+8)));
    u64 path_size = sizeof(httpReq.path);
    path_size = path_size < path_len ? path_size : path_len;
    bpf_probe_read(&httpReq.path, path_size, path_ptr);

    // Get Request.ctx
    void *ctx_iface = 0;
    bpf_probe_read(&ctx_iface, sizeof(ctx_iface), (void *)(req_ptr+ctx_ptr_pos+8));
    struct span_context* parent_ctx = extract_context_from_req_headers(req_ptr+headers_ptr_pos);
    if(parent_ctx != NULL) {
        httpReq.psc = *parent_ctx;
        copy_byte_arrays(httpReq.psc.TraceID, httpReq.sc.TraceID, TRACE_ID_SIZE);
        generate_random_bytes(httpReq.sc.SpanID, SPAN_ID_SIZE);
    } else {
        httpReq.sc = generate_span_context();
    }
    // Write event
    bpf_map_update_elem(&context_to_http_events, &ctx_iface, &httpReq, 0);
    long res = bpf_map_update_elem(&spans_in_progress, &ctx_iface, &httpReq.sc, 0);
    return 0;
}

SEC("uprobe/ServerMux_ServeHTTP")
int uprobe_ServerMux_ServeHTTP_Returns(struct pt_regs *ctx) {
    u64 request_pos = 4;
    void* req_ptr = get_argument_by_stack(ctx, request_pos);
    void *ctx_iface = 0;
    bpf_probe_read(&ctx_iface, sizeof(ctx_iface), (void *)(req_ptr+ctx_ptr_pos+8));

    void* httpReq_ptr = bpf_map_lookup_elem(&context_to_http_events, &ctx_iface);
    struct http_request_t httpReq = {};
    bpf_probe_read(&httpReq, sizeof(httpReq), httpReq_ptr);
    httpReq.end_time = bpf_ktime_get_ns();
    bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU, &httpReq, sizeof(httpReq));
    bpf_map_delete_elem(&context_to_http_events, &ctx_iface);
    bpf_map_delete_elem(&spans_in_progress, &ctx_iface);
    return 0;
}