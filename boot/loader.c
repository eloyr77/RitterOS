#include "loader.h"
#include "console.h"
#include "elf.h"
#include "string.h"

static EFI_STATUS fail(const CHAR16 *reason, EFI_STATUS status)
{
    print(u"\r\n  kernel: ");
    print(reason);
    print(u"\r\n");
    return status;
}

static EFI_STATUS failed(const CHAR16 **why, const CHAR16 *reason, EFI_STATUS status)
{
    *why = reason;
    return status;
}

EFI_STATUS read_file(EFI_BOOT_SERVICES *bs, EFI_HANDLE image, const CHAR16 *path,
                     UINT8 **data, UINTN *size, const CHAR16 **why)
{
    static EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    static EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root, *file;
    EFI_STATUS status;

    /* Which device did the firmware load us from? Open its file system. */
    status = bs->HandleProtocol(image, &loaded_image_guid, (void **)&loaded_image);
    if (EFI_ERROR(status))
        return failed(why, u"no loaded image protocol", status);
    status = bs->HandleProtocol(loaded_image->DeviceHandle, &fs_guid, (void **)&fs);
    if (EFI_ERROR(status))
        return failed(why, u"boot device has no file system", status);
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status))
        return failed(why, u"cannot open boot volume", status);
    status = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    root->Close(root);
    if (EFI_ERROR(status))
        return failed(why, u"file not found", status);

    /* File size: seek to the end and ask for the position. */
    UINT64 file_size;
    file->SetPosition(file, EFI_FILE_POSITION_END);
    file->GetPosition(file, &file_size);
    file->SetPosition(file, 0);

    status = bs->AllocatePool(EfiLoaderData, file_size, (void **)data);
    if (EFI_ERROR(status)) {
        file->Close(file);
        return failed(why, u"out of memory for the file", status);
    }
    UINTN done = 0;
    while (done < file_size) {
        UINTN chunk = file_size - done;
        status = file->Read(file, &chunk, *data + done);
        if (EFI_ERROR(status) || chunk == 0)
            break;
        done += chunk;
    }
    file->Close(file);
    if (done != file_size) {
        bs->FreePool(*data);
        return failed(why, u"read error", EFI_ERROR(status) ? status : EFI_LOAD_ERROR);
    }
    *size = file_size;
    return EFI_SUCCESS;
}

EFI_STATUS load_kernel(EFI_BOOT_SERVICES *bs, const UINT8 *file, UINTN size,
                       LOADED_KERNEL *kernel)
{
    EFI_STATUS status = EFI_LOAD_ERROR;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)file;
    if (size < sizeof(*eh) || eh->e_magic != ELF_MAGIC) {
        return fail(u"not an ELF file", status);
    }
    if (eh->e_class != ELFCLASS64 || eh->e_data != ELFDATA2LSB ||
        eh->e_machine != ELF_MACHINE_NATIVE || eh->e_type != ET_EXEC) {
        return fail(u"not a little-endian ELF executable for this CPU", status);
    }
    if (eh->e_phentsize < sizeof(Elf64_Phdr) ||
        eh->e_phoff + (UINT64)eh->e_phnum * eh->e_phentsize > size) {
        return fail(u"bad program header table", status);
    }

    /* Find the virtual address range covered by all loadable segments. */
    UINT64 lo = ~0ULL, hi = 0;
    UINTN segments = 0;
    for (UINT16 i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(file + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
            continue;
        if (ph->p_filesz > ph->p_memsz || ph->p_offset + ph->p_filesz > size) {
            return fail(u"segment outside the file", status);
        }
        if (ph->p_vaddr < lo)
            lo = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > hi)
            hi = ph->p_vaddr + ph->p_memsz;
        segments++;
    }
    if (segments == 0) {
        return fail(u"no loadable segments", status);
    }
    lo &= ~(UINT64)(EFI_PAGE_SIZE - 1);
    hi = (hi + EFI_PAGE_SIZE - 1) & ~(UINT64)(EFI_PAGE_SIZE - 1);
    if (eh->e_entry < lo || eh->e_entry >= hi) {
        return fail(u"entry point outside the image", status);
    }

    /* Any free physical pages will do: the page tables map them to the
     * virtual addresses the kernel was linked for. */
    EFI_PHYSICAL_ADDRESS phys;
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderCode, (hi - lo) / EFI_PAGE_SIZE, &phys);
    if (EFI_ERROR(status)) {
        return fail(u"not enough memory for the image", status);
    }

    /* Zero everything (this covers .bss), then copy each segment's bytes to
     * the same offset from the start as in the virtual layout. */
    memset((void *)phys, 0, hi - lo);
    for (UINT16 i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(file + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type == PT_LOAD && ph->p_filesz > 0)
            memcpy((void *)(phys + (ph->p_vaddr - lo)), file + ph->p_offset, ph->p_filesz);
    }

    kernel->entry = eh->e_entry;
    kernel->virt_base = lo;
    kernel->phys_base = phys;
    kernel->size = hi - lo;
    kernel->file_size = size;
    kernel->segments = segments;
    return EFI_SUCCESS;
}
