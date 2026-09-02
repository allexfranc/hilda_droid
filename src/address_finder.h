

#ifndef HILDA_DROID_ADDRESS_FINDER_H
#define HILDA_DROID_ADDRESS_FINDER_H

uintptr_t find_module_base(int pid, const char *module_name);
uintptr_t find_dlopen_offset();
uintptr_t find_created_vms_offset();

#endif

