#import <Foundation/Foundation.h>

#import "include/unixsock_ffi.h"

__attribute__((used)) static void* const kUnixsockKeepAliveSymbols[] = {
    (void*)&unixsock_initialize_dart_api,
    (void*)&unixsock_connect,
    (void*)&unixsock_bind_server,
    (void*)&unixsock_write,
    (void*)&unixsock_shutdown_write,
    (void*)&unixsock_destroy,
    (void*)&unixsock_set_option,
    (void*)&unixsock_get_raw_option,
    (void*)&unixsock_set_raw_option,
    (void*)&unixsock_pending_write_bytes,
    (void*)&unixsock_free_string,
};

@interface UnixsockRetention : NSObject
@end

@implementation UnixsockRetention

+ (void)load {
  (void)kUnixsockKeepAliveSymbols;
}

@end
