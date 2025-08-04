/**
 * This struct pairs a backend's name with its table of operations (vtable).
 */
typedef struct {
    const char* name;
    const hw_ops_t* ops;
} backend_entry_t;



