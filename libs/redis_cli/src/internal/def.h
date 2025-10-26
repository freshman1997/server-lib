#ifndef ___YUAN_REDIS_INTERNAL_DEF_H__
#define ___YUAN_REDIS_INTERNAL_DEF_H__

namespace yuan::redis 
{
    // DefaultBufferSize is the default size for read/write buffers (32 KiB).
    constexpr int default_buffer_size = 32 * 1024;

    // redis resp protocol data type.
	static const char resp_status    = '+'; // +<string>\r\n
	static const char resp_error     = '-'; // -<string>\r\n
	static const char resp_string    = '$'; // $<length>\r\n<bytes>\r\n
	static const char resp_int       = ':'; // :<number>\r\n
	static const char resp_null       = '_'; // _\r\n
	static const char resp_float     = ','; // ,<floating-point-number>\r\n (golang float)
	static const char resp_bool      = '#'; // true: #t\r\n false: #f\r\n
	static const char resp_blob_error = '!'; // !<length>\r\n<bytes>\r\n
	static const char resp_verbatim  = '='; // =<length>\r\nFORMAT:<bytes>\r\n
	static const char resp_bigInt    = '('; // (<big number>\r\n
	static const char resp_array     = '*'; // *<len>\r\n... (same as resp2)
	static const char resp_map       = '%'; // %<len>\r\n(key)\r\n(value)\r\n... (golang map)
	static const char resp_set       = '~'; // ~<len>\r\n... (same as Array)
	static const char resp_attr      = '|'; // |<len>\r\n(key)\r\n(value)\r\n... + command reply
	static const char resp_push      = '>'; // ><len>\r\n... (same as Array)
}

#endif // ___YUAN_REDIS_INTERNAL_DEF_H__