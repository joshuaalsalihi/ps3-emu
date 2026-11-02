#include "hle/linker.h"
#include "hle/nid.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("NIDs match the ones in PSL1GHT's sysPrxForUser import table") {
    // Expected values: PSL1GHT, ppu/sprx/liblv2/exports.h.
    CHECK(hle::nid("sys_process_exit") == 0xe6f2c1e7);
    CHECK(hle::nid("sys_ppu_thread_create") == 0x24a1ea07);
    CHECK(hle::nid("sys_lwmutex_lock") == 0x1573dc3f);
    CHECK(hle::nid("sys_lwcond_signal_to") == 0x52aadadf);
    CHECK(hle::nid("sys_mmapper_allocate_memory_from_container") == 0x70258515);
    CHECK(hle::nid("_sys_printf") == 0x9f04f7af);
}

TEST_CASE("NIDs match the ones in PSL1GHT's cellGcmSys and cellSysutil stub libraries") {
    // Expected values: the *_fnid symbols in PSL1GHT's libgcm_sys.a and libsysutil.a.
    CHECK(hle::nid("_cellGcmInitBody") == 0x15bae46b);
    CHECK(hle::nid("cellGcmAddressToOffset") == 0x21ac3697);
    CHECK(hle::nid("_cellGcmSetFlipCommand") == 0x21397818);
    CHECK(hle::nid("cellGcmSetWaitFlip") == 0x983fb9aa);
    CHECK(hle::nid("cellVideoOutConfigure") == 0x0bae8772);
    CHECK(hle::nid("cellVideoOutGetState") == 0x887572d5);
}

TEST_CASE("HLE function names are unique within each module") {
    for (const hle::ModuleInfo& module : hle::modules()) {
        for (std::size_t i = 0; i < module.functions.size(); ++i) {
            for (std::size_t j = i + 1; j < module.functions.size(); ++j) {
                INFO(module.functions[i].name);
                CHECK(hle::nid(module.functions[i].name) != hle::nid(module.functions[j].name));
            }
        }
    }
}
