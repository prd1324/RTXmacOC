/*
 * RTXProbe.hpp — kext-драйвер RTXmacOC, слой 1 (PCIe bring-up).
 *
 * Матчится на NVIDIA RTX 4070 Super (10DE:2783) как IOPCIDevice, мапит BAR0
 * и читает NV_PMC_BOOT_0. Канал доставки — kext, инжектируемый OpenCore
 * (см. ARCHITECTURE.md, раздел 4). Это первый реальный разговор с картой.
 */
#ifndef RTXMACOC_RTXPROBE_HPP
#define RTXMACOC_RTXPROBE_HPP

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>

class RTXProbe : public IOService {
    OSDeclareDefaultStructors(RTXProbe)

public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;

private:
    IOPCIDevice  *fPci  = nullptr;
    IOMemoryMap  *fBar0 = nullptr;
    bool          fIsAda = false; /* выставляется при успешном декоде PMC_BOOT_0 */

    bool mapBar0AndReadBoot0(void);
};

#endif /* RTXMACOC_RTXPROBE_HPP */
