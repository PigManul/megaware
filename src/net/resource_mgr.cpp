#include "resource_mgr.h"

namespace {
    // Владельцы клеймов; nullptr = свободно. Один владелец на ресурс:
    // фичи работают из главного цикла (один поток), heap/локи не нужны.
    const char* ownerHttp80 = nullptr;
    const char* ownerDns53 = nullptr;
}

namespace ResMgr {

bool claim(Resource r, const char* owner) {
    if (owner == nullptr) return false;
    const char** slot = (r == Resource::Http80) ? &ownerHttp80 : &ownerDns53;
    if (*slot == nullptr || strcmp(*slot, owner) == 0) {
        *slot = owner;
        return true;
    }
    return false;
}

void release(Resource r, const char* owner) {
    if (owner == nullptr) return;
    const char** slot = (r == Resource::Http80) ? &ownerHttp80 : &ownerDns53;
    if (*slot != nullptr && strcmp(*slot, owner) == 0) *slot = nullptr;
}

bool isHeld(Resource r) {
    return (r == Resource::Http80) ? ownerHttp80 != nullptr : ownerDns53 != nullptr;
}

} // namespace ResMgr
