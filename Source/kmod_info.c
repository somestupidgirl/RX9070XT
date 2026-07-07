//
//  kmod_info.c
//  RX9070XT
//
//  Hand-written replacement for the kmod_info glue that Xcode's kext build
//  normally generates from CreateKModInfo.perl. Wires the kernel module entry
//  points to Lilu's plugin bootstrap (RX9070XT_kern_start / _kern_stop from
//  plugin_start.cpp).
//
//  Link order matters: <objects> -lkmodc++ kmod_info.o -lkmod
//    * libkmodc++ provides _start/_stop that run C++ static constructors and
//      then jump to _realmain / _antimain.
//    * this file defines _realmain / _antimain and the kmod_info struct.
//

#include <mach/mach_types.h>
#include <libkern/OSKextLib.h>

// Provided by libkmodc++ (cplus_start.c / cplus_stop.c): runs constructors,
// then calls _realmain, and destructors after _antimain.
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

// Provided by Lilu's plugin_start.cpp (compiled into this kext).
extern kern_return_t RX9070XT_kern_start(kmod_info_t *ki, void *data);
extern kern_return_t RX9070XT_kern_stop(kmod_info_t *ki, void *data);

KMOD_EXPLICIT_DECL(com.hackintosh.RX9070XT, "0.0.1", _start, _stop)

__private_extern__ kmod_start_func_t *_realmain = RX9070XT_kern_start;
__private_extern__ kmod_stop_func_t  *_antimain = RX9070XT_kern_stop;
