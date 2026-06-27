/*
 * RTXProbeDext.cpp — DriverKit-реализация bring-up слоя 1.
 *
 * Через PCIDriverKit: матчится на 10DE:2783, включает Memory/Bus Master,
 * читает NV_PMC_BOOT_0 из BAR0 через MemoryRead32 и декодит чип.
 *
 * Ограничения DriverKit (см. ARCHITECTURE.md, раздел 3): к BAR обращаемся
 * через MemoryRead*/MemoryWrite*, а не сырым указателем — прямой доступ к BAR0
 * крэшит. memoryIndex для нужного BAR получаем из GetBARInfo.
 */
#include <os/log.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>
#include <PCIDriverKit/PCIDriverKit.h>

#include "RTXProbeDext.h"
#include "../../ada_regs.h"

#define PCI_REG_COMMAND          0x04
#define PCI_COMMAND_MEMORY_SPACE 0x0002
#define PCI_COMMAND_BUS_MASTER   0x0004

struct RTXProbeDext_IVars {
    IOPCIDevice *pci;
};

bool RTXProbeDext::init()
{
    if (!super::init()) {
        return false;
    }
    ivars = IONewZero(RTXProbeDext_IVars, 1);
    return ivars != nullptr;
}

void RTXProbeDext::free()
{
    if (ivars) {
        IOSafeDeleteNULL(ivars, RTXProbeDext_IVars, 1);
    }
    super::free();
}

kern_return_t IMPL(RTXProbeDext, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    ivars->pci = OSDynamicCast(IOPCIDevice, provider);
    if (ivars->pci == nullptr) {
        os_log(OS_LOG_DEFAULT, "RTXProbeDext: provider is not IOPCIDevice");
        Stop(provider, SUPERDISPATCH);
        return kIOReturnNoDevice;
    }

    ret = ivars->pci->Open(this, 0);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "RTXProbeDext: Open failed 0x%x", ret);
        Stop(provider, SUPERDISPATCH);
        return ret;
    }

    uint16_t vendor = 0, device = 0;
    ivars->pci->ConfigurationRead16(0x00, &vendor);
    ivars->pci->ConfigurationRead16(0x02, &device);
    os_log(OS_LOG_DEFAULT, "RTXProbeDext: matched %04x:%04x", vendor, device);

    /* Включаем Memory Space + Bus Master. */
    uint16_t cmd = 0;
    ivars->pci->ConfigurationRead16(PCI_REG_COMMAND, &cmd);
    cmd |= (PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER);
    ivars->pci->ConfigurationWrite16(PCI_REG_COMMAND, cmd);

    /* Находим memoryIndex для BAR0. */
    uint8_t  memoryIndex = 0;
    uint64_t barSize     = 0;
    uint8_t  barType     = 0;
    ret = ivars->pci->GetBARInfo(0, &memoryIndex, &barSize, &barType);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "RTXProbeDext: GetBARInfo(0) failed 0x%x", ret);
        ivars->pci->Close(this, 0);
        ivars->pci = nullptr;
        Stop(provider, SUPERDISPATCH);
        return ret;
    }
    os_log(OS_LOG_DEFAULT, "RTXProbeDext: BAR0 memIndex=%u size=0x%llx",
           memoryIndex, barSize);

    /* Читаем NV_PMC_BOOT_0 (offset 0x0). */
    uint32_t boot0 = 0;
    ret = ivars->pci->MemoryRead32(memoryIndex, NV_PMC_BOOT_0, &boot0);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "RTXProbeDext: MemoryRead32(PMC_BOOT_0) failed 0x%x", ret);
        ivars->pci->Close(this, 0);
        ivars->pci = nullptr;
        Stop(provider, SUPERDISPATCH);
        return ret;
    }

    os_log(OS_LOG_DEFAULT, "RTXProbeDext: NV_PMC_BOOT_0 = 0x%08x", boot0);

    if (!nv_boot0_plausible(boot0)) {
        os_log(OS_LOG_DEFAULT, "RTXProbeDext: PMC_BOOT_0 implausible (0x%08x)", boot0);
    } else {
        nv_boot0_t b = nv_decode_boot0(boot0);
        os_log(OS_LOG_DEFAULT, "RTXProbeDext: arch=0x%02x impl=0x%x chipset=0x%03x rev=%u.%u -> %s / %s",
               b.architecture, b.implementation, b.chipset,
               b.major_revision, b.minor_revision,
               nv_arch_name(b.architecture), nv_chipset_name(b.chipset));
        if (b.architecture == NV_ARCHITECTURE_AD) {
            os_log(OS_LOG_DEFAULT, "RTXProbeDext: *** ВИЖУ Ada %s — стена пробита ***",
                   nv_chipset_name(b.chipset));
        }
    }

    RegisterService();
    return kIOReturnSuccess;
}

kern_return_t IMPL(RTXProbeDext, Stop)
{
    if (ivars->pci != nullptr) {
        ivars->pci->Close(this, 0);
        ivars->pci = nullptr;
    }
    os_log(OS_LOG_DEFAULT, "RTXProbeDext: Stop");
    return Stop(provider, SUPERDISPATCH);
}
