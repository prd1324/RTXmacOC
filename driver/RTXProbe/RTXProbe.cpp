/*
 * RTXProbe.cpp — реализация kext-драйвера слоя 1.
 *
 * Что делает start():
 *   1. Кастит provider к IOPCIDevice (нас приматчил IOPCIFamily по 10DE:2783).
 *   2. Включает Memory Space + Bus Master в command-регистре.
 *   3. Мапит BAR0 (регистровый MMIO-блок) в адресное пространство ядра.
 *   4. Читает NV_PMC_BOOT_0 (offset 0x0) и декодит architecture/implementation.
 *   5. Печатает «вижу Ada AD104» в системный лог.
 *
 * Регистры не пишутся, прошивка не трогается — это только разведка слоя 1.
 *
 * Сборка/загрузка: см. driver/RTXProbe/README.md (Xcode kext target + OpenCore
 * инжект с релаксом SIP). Проверить на железе — это закрытие Недели 4.
 */
#include "RTXProbe.hpp"
#include "FwsecRun.hpp"

#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>

#include "../../ada_regs.h"

/* PCI config space. */
#define PCI_REG_COMMAND          0x04
#define PCI_COMMAND_MEMORY_SPACE 0x0002
#define PCI_COMMAND_BUS_MASTER   0x0004

#define super IOService
OSDefineMetaClassAndStructors(RTXProbe, IOService)

bool RTXProbe::start(IOService *provider)
{
    if (!super::start(provider)) {
        return false;
    }

    fPci = OSDynamicCast(IOPCIDevice, provider);
    if (fPci == nullptr) {
        IOLog("RTXProbe: provider is not an IOPCIDevice — abort\n");
        return false;
    }

    /* Подтверждаем, с кем говорим, по config space. */
    uint16_t vendor = fPci->configRead16(0x00);
    uint16_t device = fPci->configRead16(0x02);
    IOLog("RTXProbe: matched PCI device %04x:%04x\n", vendor, device);

    /* Открываем устройство и включаем доступ к памяти + bus master. */
    if (!fPci->open(this)) {
        IOLog("RTXProbe: IOPCIDevice::open failed\n");
        return false;
    }

    fPci->setMemoryEnable(true);
    fPci->setBusMasterEnable(true);

    uint16_t cmd = fPci->configRead16(PCI_REG_COMMAND);
    cmd |= (PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER);
    fPci->configWrite16(PCI_REG_COMMAND, cmd);

    bool ok = mapBar0AndReadBoot0();

    /*
     * Слой 2 (задача 8): прогон FWSEC-FRTS. Выполняем, пока устройство открыто
     * (bus master включён — нужен для DMA в Falcon). Запускаем только на Ada
     * (декод PMC_BOOT_0 успешен), чтобы не трогать чужие чипы. Это hardware-
     * операция (reset GSP-Falcon + создание WPR2) — ради неё и грузится стенд.
     */
    if (ok && fIsAda) {
        IOVirtualAddress base = fBar0->getVirtualAddress();
        IOLog("RTXProbe: === запуск FWSEC-FRTS (layer 2) ===\n");
        IOReturn fr = RTXRunFwsecFrts(fPci, reinterpret_cast<volatile void *>(base));
        IOLog("RTXProbe: RTXRunFwsecFrts вернул 0x%08x (%s)\n",
              fr, (fr == kIOReturnSuccess) ? "OK" : "FAIL");
    }

    fPci->close(this);

    if (!ok) {
        IOLog("RTXProbe: BAR0 read failed\n");
        return false;
    }

    registerService();
    return true;
}

bool RTXProbe::mapBar0AndReadBoot0(void)
{
    /*
     * BAR0 = config register kIOPCIConfigBaseAddress0 (0x10). Это регистровый
     * MMIO-блок карты. Мапим его в виртуальное адресное пространство ядра.
     */
    fBar0 = fPci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (fBar0 == nullptr) {
        IOLog("RTXProbe: mapDeviceMemoryWithRegister(BAR0) returned null\n");
        return false;
    }

    IOVirtualAddress base = fBar0->getVirtualAddress();
    IOByteCount      len  = fBar0->getLength();
    IOLog("RTXProbe: BAR0 mapped at kv=0x%llx len=0x%llx\n",
          (uint64_t)base, (uint64_t)len);

    if (base == 0 || len < 4) {
        IOLog("RTXProbe: BAR0 mapping invalid\n");
        return false;
    }

    /*
     * Читаем NV_PMC_BOOT_0 (offset 0x0). Регистры NVIDIA little-endian, x86 тоже
     * little-endian — читаем напрямую через volatile. OSReadLittleInt32 на всякий
     * случай нормализует порядок байт.
     */
    volatile uint32_t *mmio = reinterpret_cast<volatile uint32_t *>(base);
    uint32_t boot0 = OSReadLittleInt32((void *)mmio, NV_PMC_BOOT_0);

    IOLog("RTXProbe: NV_PMC_BOOT_0 = 0x%08x\n", boot0);

    if (!nv_boot0_plausible(boot0)) {
        IOLog("RTXProbe: PMC_BOOT_0 implausible (0x%08x) — карта не запитана "
              "или маппинг не на регистры\n", boot0);
        return false;
    }

    nv_boot0_t b = nv_decode_boot0(boot0);
    IOLog("RTXProbe: chip arch=0x%02x impl=0x%x chipset=0x%03x rev=%u.%u -> %s / %s\n",
          b.architecture, b.implementation, b.chipset,
          b.major_revision, b.minor_revision,
          nv_arch_name(b.architecture), nv_chipset_name(b.chipset));

    if (b.architecture == NV_ARCHITECTURE_AD) {
        fIsAda = true;
        IOLog("RTXProbe: *** ВИЖУ Ada %s ***\n",
              nv_chipset_name(b.chipset));
    } else {
        IOLog("RTXProbe: arch не Ada (0x%02x) — проверить чип/раскладку полей\n",
              b.architecture);
    }

    return true;
}

void RTXProbe::stop(IOService *provider)
{
    if (fBar0 != nullptr) {
        fBar0->release();
        fBar0 = nullptr;
    }
    fPci = nullptr;
    IOLog("RTXProbe: stop\n");
    super::stop(provider);
}
