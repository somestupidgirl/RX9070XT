//
//  kmod_info.c
//  RDNA4FB
//
//  Hand-written replacement for the kmod_info glue that Xcode's kext build
//  normally generates from CreateKModInfo.perl.
//
//  Link order matters: <objects> -lkmodc++ kmod_info.o -lkmod
//    * libkmodc++ provides _start/_stop that run C++ static constructors and
//      then jump to _realmain / _antimain.
//    * this file defines _realmain / _antimain and the kmod_info struct.
//
//  The kext is a plain IOKit driver (no Lilu linkage): all work happens in
//  the RDNA4FB class instantiated by IOKit matching, so the module entry
//  points have nothing to do.
//

#include <mach/mach_types.h>
#include <libkern/OSKextLib.h>

// Provided by libkmodc++ (cplus_start.c / cplus_stop.c): runs constructors,
// then calls _realmain, and destructors after _antimain.
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

static kern_return_t rdna4_kmod_start(kmod_info_t *ki, void *data) {
	(void)ki; (void)data;
	return KERN_SUCCESS;
}

static kern_return_t rdna4_kmod_stop(kmod_info_t *ki, void *data) {
	(void)ki; (void)data;
	return KERN_SUCCESS;
}

KMOD_EXPLICIT_DECL(com.hackintosh.RDNA4FB, "0.0.1", _start, _stop)

__private_extern__ kmod_start_func_t *_realmain = rdna4_kmod_start;
__private_extern__ kmod_stop_func_t  *_antimain = rdna4_kmod_stop;
