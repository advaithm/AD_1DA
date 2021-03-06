#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libelf.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <inttypes.h>
#include <gelf.h>
#include <stdarg.h>
#include <getopt.h>
#include <ctype.h>
#include <elf.h>
#include <bfd.h>
#include <pthread.h> 

#include "include.h"

#include <keystone/keystone.h>
#include <capstone/capstone.h>

/*

   We have :

    // --------------------------// |
                                    |
    //        Elf Header         // |
                                    |
    // --------------------------// |
                                    |
    //    Program Header Table   // | =========> 1er PT_LOAD
                                    |
    // --------------------------// |
                                    |
    //       Nos sections        // | --------------------------------
                                    |
    // --------------------------// | ========> 2eme PT_LOAD 
    //    Section header table   //   ========> not mapped (useless)
    // --------------------------//

    Algo : 
        edit ep,
        On add len second PT_LOAD to len_scnd_pt_load += len_sec (filesisz + memsz) + edit exec permissions
        edit shoff,
        Add code at scnd_phdr->p_offset + scnd_phdr->p_memsz

*/

// ========================== Check if the file is an elf ============================

int is_elf(unsigned char *eh_ptr){

    if ((unsigned char)eh_ptr[EI_MAG0] != 0x7F ||
        (unsigned char)eh_ptr[EI_MAG1] != 'E' ||
        (unsigned char)eh_ptr[EI_MAG2] != 'L' || 
        (unsigned char)eh_ptr[EI_MAG3] != 'F'){
        return -1;
    }

    return 0;
}

// ========================== New main function ============================

int add_section_ovrwrte_ep_inject_code(const char *filename, const char *name_sec, unsigned char *stub, ssize_t len_stub, bool pie, bool meta){

    struct stat stat_file = {0};
    unsigned char *file_ptr;
    int fd=0;
    int random_key = 0;
    ssize_t len_text = 0;

    // Elf structure

    Elf64_Ehdr *eh_ptr = NULL;
    
    // ====

    if ((fd = open(filename, O_RDWR)) == -1)
    {
        perror("[ERROR] Open\n");
        return -1;
    }

    // ====

    if (fstat(fd, &stat_file))
    {
        printf("[ERROR] fstat has failed1\n");
        return -1;
    }


    file_ptr = (unsigned char *)mmap(0, stat_file.st_size, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
    
    if (file_ptr == MAP_FAILED)
    {
        printf("[ERROR] mmap has failed\n");
        return 1;
    }



    // Initialisation for few structs

    eh_ptr = (Elf64_Ehdr *)file_ptr;

    Elf64_Phdr *buffer_mdata_ph[eh_ptr->e_phnum];
    Elf64_Shdr *buffer_mdata_sh[eh_ptr->e_shnum];

    parse_shdr(eh_ptr, buffer_mdata_sh);
    parse_phdr(eh_ptr, buffer_mdata_ph);

    char *sh_name_buffer[eh_ptr->e_shnum];
    
    off_t offset = 0;
    Elf64_Shdr *shstrtab_header;

    shstrtab_header = (Elf64_Shdr *)((char *)file_ptr + eh_ptr->e_shoff + eh_ptr->e_shentsize * eh_ptr->e_shstrndx);

    char *shstrndx = (char *)file_ptr + shstrtab_header->sh_offset;

    for (size_t i = 0; i < eh_ptr->e_shnum; i++){

		sh_name_buffer[i] = (char *)shstrndx + buffer_mdata_sh[i]->sh_name;

	}

    // ==========

    // The first pt_load

    Elf64_Phdr *phdr_fst_pt = search_fst_pt_load(eh_ptr, buffer_mdata_ph);

    // We search the seconf pt_load

    Elf64_Phdr *scnd_pt_load = NULL;

    for (size_t i = 0; i < eh_ptr->e_phnum; i++)
    {
        if (buffer_mdata_ph[i]->p_type == PT_LOAD && buffer_mdata_ph[i]->p_flags == 0x5)
        {
            continue;
        }
        else if (buffer_mdata_ph[i]->p_type == PT_LOAD)
        {
            scnd_pt_load = buffer_mdata_ph[i];
            printf("Second pt_load is found at 0x%lx\n", scnd_pt_load->p_offset);
        }
        
    }

    if (scnd_pt_load == NULL)
    {
        printf("[_] Second pt_load not found\n");
        printf("Exiting\n");

        // ============

        if (munmap(file_ptr, stat_file.st_size) != 0){
		    exit(-1);
	    }

        return 0;
    }
    
    
    unsigned char *malloc_fst_pt = malloc(phdr_fst_pt->p_filesz + (scnd_pt_load->p_offset - phdr_fst_pt->p_filesz) + scnd_pt_load->p_filesz + len_stub);

    memcpy(malloc_fst_pt, file_ptr, phdr_fst_pt->p_filesz + (scnd_pt_load->p_offset - phdr_fst_pt->p_filesz) + scnd_pt_load->p_filesz);


    // ==========


    ssize_t len_data = stat_file.st_size - (phdr_fst_pt->p_filesz + (scnd_pt_load->p_offset - phdr_fst_pt->p_filesz) + scnd_pt_load->p_filesz); // En vrai la taille change pas l'offset va changer par contre

    Elf64_Shdr *data_nxt_pt = malloc(len_data);

    memcpy(data_nxt_pt, file_ptr + (phdr_fst_pt->p_filesz + (scnd_pt_load->p_offset - phdr_fst_pt->p_filesz) + scnd_pt_load->p_filesz), len_data);

    // ==========

    
    unsigned char *address_to_inject =  m_new_section(malloc_fst_pt, (unsigned char *)data_nxt_pt, buffer_mdata_ph, len_stub, stat_file.st_size, len_data);
    
    Elf64_Ehdr *tmp_eh_ptr = (Elf64_Ehdr *)address_to_inject; // header temporarire avec l'adresse du truc que on va inject

    // Pie or not

    off_t offset_text = search_section_name(sh_name_buffer, (Elf64_Ehdr *)address_to_inject, buffer_mdata_sh, ".text", &len_text);

    // offset of the .text

    if (pie == true)
    {

        // Either the pie or not, so we patch the syub with differents values

        if (patch_target(stub, (long)0x1111111111111111, len_stub, (long)offset_text) || patch_target(stub, 0x3333333333333333, len_stub, (long)(scnd_pt_load->p_vaddr + scnd_pt_load->p_filesz)) == -1) //|| patch_target(stub, 0x22222222, len_stub, (long)0) == -1)
        {
            printf("The stub cannot be patched because the pattern 0x1111111111111111, 0x3333333333333333 can't be found\n");
            exit(-1);
        }

        // overwrite l'ep

        tmp_eh_ptr->e_entry = scnd_pt_load->p_vaddr + scnd_pt_load->p_filesz; // Vu que là on est au niveau de la mémoire, on manipule l'addr virtuelle et sa memoty size
        printf("The binary has the pie !\n");
        printf("Entry point overwritten : %lx\n", tmp_eh_ptr->e_entry);

        // Check if the binary will be metamorphic

        if (meta == true){

            // 1. Patch the stub with specials patterns

            srand(time(NULL)); 
            random_key = 1 + rand() % (255 - 1 + 1);

            printf("Code cave length : 0x%lx\n", scnd_pt_load->p_offset - phdr_fst_pt->p_filesz);
            printf("This binary will have %ld possible hashes\n", 256 * (scnd_pt_load->p_offset - phdr_fst_pt->p_filesz + 1));

            if (patch_target(stub, (long)0x4444444444444444, len_stub, (long)random_key) || patch_target(stub, (long)0x5555555555555555, len_stub, (long)len_stub) || patch_target(stub, (long)0x6666666666666666, len_stub, (long)scnd_pt_load->p_offset + scnd_pt_load->p_filesz) || patch_target(stub, (long)0x7777777777777777, len_stub, (long)phdr_fst_pt->p_memsz) || patch_target(stub, (long)0x1111111111111111, len_stub, (long)offset_text) || patch_target(stub, (long)0x8888888888888888, len_stub, (long)len_text) || patch_target(stub, (long)0x9999999999999999, len_stub, (long)scnd_pt_load->p_offset) == -1)
            {
                printf("The stub cannot be patched because the pattern 0x4444444444444444 can't be found\n");
                exit(-1);
            }

        }

    }
    else
    {
        // Search the base address

        long base_address = search_base_addr(buffer_mdata_ph, eh_ptr);
        tmp_eh_ptr->e_entry = scnd_pt_load->p_vaddr + scnd_pt_load->p_filesz;
        printf("The base address of the target binary is 0x%lx\n", base_address);
        printf("Entry point overwritten : 0x%lx\n", tmp_eh_ptr->e_entry);

        if (patch_target(stub, 0x1111111111111111, len_stub, (long)base_address) == -1)
        {
            printf("The stub cannot be patched because the pattern 0x1111111111111111 can't be found\n"); // 0x3333333333333333
            exit(-1);
        }

        if (meta == true){
            srand(time(NULL)); 
            random_key = 1 + rand() % (255 - 1 + 1);

            printf("Code cave length : 0x%lx\n", scnd_pt_load->p_offset - phdr_fst_pt->p_filesz);
            printf("This binary will have %ld possible hashes\n", 265 * (scnd_pt_load->p_offset - phdr_fst_pt->p_filesz + 1));

            if (patch_target(stub, (long)0x4444444444444444, len_stub, (long)random_key) || patch_target(stub, (long)0x5555555555555555, len_stub, (long)len_stub) || patch_target(stub, (long)0x6666666666666666, len_stub, (long)scnd_pt_load->p_offset + scnd_pt_load->p_filesz) || patch_target(stub, (long)0x7777777777777777, len_stub, (long)phdr_fst_pt->p_memsz) || patch_target(stub, (long)0x1111111111111111, len_stub, (long)offset_text) || patch_target(stub, (long)0x8888888888888888, len_stub, (long)len_text) || patch_target(stub, (long)0x1111111111111111, len_stub, (long)offset_text) || patch_target(stub, (long)0x9999999999999999, len_stub, (long)scnd_pt_load->p_offset) == -1)
            {
                printf("The stub cannot be patched because the pattern 0x4444444444444444 can't be found\n");
                exit(-1);
            }
        }

    }

    if (inject_section(stub, len_stub, address_to_inject, scnd_pt_load->p_offset + scnd_pt_load->p_filesz))
    {
        perror("[ERROR] Exit \n");
        return -1;
    }
    
    if (meta == true){
        x_pack_text(address_to_inject + offset_text, len_text, random_key);
    }

    // Create a new file

    char *name_file_dumped = strcat((char *)filename, name_sec);

    printf("[*] Generating a new %s executable file\n", name_file_dumped);

    int file_dumped = open(name_file_dumped,  O_RDWR | O_CREAT, 0777);

    if (file_dumped == -1)
    {
        perror("[ERROR] open\n");
        close(file_dumped);
    }
    
    // We write all our malloc in the file

    write(file_dumped, address_to_inject, len_stub + scnd_pt_load->p_offset + scnd_pt_load->p_filesz + len_data);

    printf("Bytes injected at 0x%lx: \n", scnd_pt_load->p_vaddr + scnd_pt_load->p_memsz);
    printf("\t");

    for (size_t i = 0; i < len_stub; i++)
    {
        printf("%x", *(address_to_inject + scnd_pt_load->p_offset + scnd_pt_load->p_filesz + i));
    }
    
    printf("\n");

    printf("Length of the stub : 0x%lx\n", len_stub);
        

    // =====

    if (munmap(file_ptr, stat_file.st_size) != 0){
		exit(-1);
	}

    // close an free

    close(fd);
    close(file_dumped);

    free(data_nxt_pt);
    free(address_to_inject);

    return 0;
}

// ===========================================================================================================


unsigned char *init_map_and_get_stub(const char *stub_file, ssize_t *len_stub, bool disass_or_not){

    struct stat stat_file = {0};

    int fd = open(stub_file, O_RDWR);

    if (fstat(fd, &stat_file))
    {
        printf("[ERROR] fstat has failed\n");
        return NULL;
    }

    unsigned char *stub_ptr = mmap(0, stat_file.st_size, PROT_READ, MAP_SHARED, fd, 0);

    *len_stub = stat_file.st_size;

    if (stub_ptr == MAP_FAILED)
    {
        printf("[ERROR] mmap has failed\n");
        return NULL;
    }

    Elf64_Ehdr *eh_ptr = (Elf64_Ehdr *)stub_ptr;

    Elf64_Phdr *buffer_mdata_ph[eh_ptr->e_phnum];
    Elf64_Shdr *buffer_mdata_sh[eh_ptr->e_shnum];

    parse_shdr(eh_ptr, buffer_mdata_sh);
    parse_phdr(eh_ptr, buffer_mdata_ph);

    char *sh_name_buffer[eh_ptr->e_shnum];
    
    off_t offset = 0;
	Elf64_Shdr *shstrtab_header;

    // ================================================

    shstrtab_header = (Elf64_Shdr *)((char *)stub_ptr + eh_ptr->e_shoff + eh_ptr->e_shentsize * eh_ptr->e_shstrndx);

    const char *shstrndx = (const char *)stub_ptr + shstrtab_header->sh_offset;

    for (size_t i = 0; i < eh_ptr->e_shnum; i++){

		sh_name_buffer[i] = (char *)shstrndx + buffer_mdata_sh[i]->sh_name;

	}

    // ================================================

    // Search base_address

    off_t offset_entry = 0;

    if (!has_pie_or_not(buffer_mdata_ph, eh_ptr))
    {
        offset_entry = eh_ptr->e_entry; // pie
    }
    else
    {
        uint64_t base_address = search_base_addr(buffer_mdata_ph, eh_ptr);
        offset_entry = eh_ptr->e_entry - base_address;
    }
    
    // =================================================

    off_t offset_text = search_section_name(sh_name_buffer, eh_ptr, buffer_mdata_sh, ".text", len_stub);
    
    int size_stub_malloc = *len_stub;

    unsigned char *text_stub = malloc(size_stub_malloc);

    memcpy(text_stub, stub_ptr + offset_text, size_stub_malloc);

    printf("Raw executables bytes in the stub : \n");
    printf("\t");

    for (size_t i = 0; i < size_stub_malloc; i++)
    {
        printf("%x", *(text_stub + i));
    }

    printf("\n");

    if (disass_or_not == false)
    {
        printf("Disassembling the stub : \n");

        disass_raw(text_stub, size_stub_malloc); // Disassembly
    }


    if (munmap(stub_ptr, stat_file.st_size))
    {
        exit(-1);
    }

    close(fd);

    return text_stub;
}

// ===========================================================================================================

unsigned char *init_map_and_get_stub_raw(const char *stub_file, ssize_t *len_stub){

    struct stat stat_file = {0};
    int fd=0;

    if((fd = open(stub_file, O_RDWR)) == -1){
        printf("[ERROR] Fatal open, %s not found\n", stub_file);
        exit(-1);
    }

    if (fstat(fd, &stat_file))
    {
        printf("[ERROR] fstat has failed\n");
        return NULL;
    }

    unsigned char *stub_ptr = mmap(0, stat_file.st_size, PROT_READ, MAP_SHARED, fd, 0);

    if (stub_ptr == MAP_FAILED)
    {
        printf("[ERROR] mmap has failed\n");
        return NULL;
    }

    *len_stub = stat_file.st_size;

    off_t offset = 0;
    off_t offset_entry = 0;

    int size_stub_malloc = *len_stub;

    printf("Instructions to inject : \n");
    printf("\t");
    printf("%s", stub_ptr);

    printf("Instructions compiled : \n");
    printf("\t");

    ks_engine *ks;
    ks_err error;
    size_t count;
    unsigned char *encode;
    size_t size;

    if((error = ks_open(KS_ARCH_X86, KS_MODE_64, &ks)) != KS_ERR_OK){
        printf("ERROR: failed on ks_open(), quit\n");
        return (unsigned char *)-1;
    }

    if (ks_asm(ks, stub_ptr, 0, &encode, &size, &count) != KS_ERR_OK)
    {
        printf("ERROR: ks_asm() failed & count = %lu, error = %u\n", count, ks_errno(ks));
    }
    else
    {
        for (size_t i = 0; i < size; i++)
        {
            printf("%2x ", *(encode + i));
        }
        
    }

    unsigned char *data_stub = malloc(size);

    printf("\n");

    *len_stub = size;

    memcpy(data_stub, encode, size);

    printf("\t");

    for (size_t i = 0; i < size; i++)
    {
        printf("%x", *(data_stub + i));
    }

    printf("\n");

    if (munmap(stub_ptr, stat_file.st_size))
    {
        exit(-1);
    }

    ks_free(encode);
    ks_close(ks);

    close(fd);

    return data_stub;
}

// ===========================================================================================================

int main_fetcher(void){


}

// ===========================================================================================================

Elf64_Shdr *elf_struct_search_section_name(Elf64_Ehdr *eh_ptr, Elf64_Shdr *buffer_mdata_sh[], const char *section, char *sh_name_buffer[]){

    for (size_t i = 0; i < eh_ptr->e_shnum; i++)
    {
        if (!strcmp(section, sh_name_buffer[i]))
        {
            return buffer_mdata_sh[i];
        }
        
    }

    return (Elf64_Shdr *)-1;
}

// ==============================================================================================================

int disass_raw(unsigned char *raw_bytes, ssize_t len_raw_code){

    csh handle;
    cs_insn *instruction;
    ssize_t count_insn;

    if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK)
    {
        return -1;
    }

    count_insn = cs_disasm(handle, raw_bytes, len_raw_code, 0x0, 0x0, &instruction);

    if (count_insn > 0)
    {
        for (size_t i = 0; i < count_insn; i++)
        {
            if (*instruction[i].op_str == '\0')
            {
                printf("\t[%s]      _\n", instruction[i].mnemonic);
            }
            else
            {
                printf("\t[%s]      %s\n", instruction[i].mnemonic, instruction[i].op_str);
            }
        }
        
        cs_free(instruction, count_insn);
    }

    cs_close(&handle);

    return 0;
}


// =====================================================================================================================================


ssize_t len_bytes(unsigned char *bytes){

    return 0;
}

// =====================================================================================================================================
