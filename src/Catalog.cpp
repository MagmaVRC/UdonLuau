#include "UdonLuau/Catalog.hpp"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace UdonLuau {

    namespace {

        struct StringHash {
            using is_transparent = void;
            size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
        };

        template <typename T>
        using StringMap = std::unordered_map<std::string, T, StringHash, std::equal_to<>>;

        std::vector<std::string> SplitParameters(std::string_view text) {
            std::vector<std::string> out;
            if (text.empty()) return out;
            size_t start = 0;
            while (true) {
                size_t sep = text.find('_', start);
                out.emplace_back(text.substr(start, sep == std::string_view::npos ? std::string_view::npos : sep - start));
                if (sep == std::string_view::npos) break;
                start = sep + 1;
            }
            return out;
        }

        bool IsGenericName(std::string_view name) {
            return name == "T" || name == "TArray" || name == "ListT";
        }

        std::string_view ShortName(std::string_view fullName) {
            size_t cut = fullName.find_last_of(".+");
            return cut == std::string_view::npos ? fullName : fullName.substr(cut + 1);
        }

        constexpr std::string_view kStandardEvents[] = {
            "FixedUpdate", "LateUpdate", "PostLateUpdate", "Start", "Update", "Interact",
            "InputDrop|boolValue:System.Boolean|args:VRC.Udon.Common.UdonInputEventArgs",
            "InputGrab|boolValue:System.Boolean|args:VRC.Udon.Common.UdonInputEventArgs",
            "InputJump|boolValue:System.Boolean|args:VRC.Udon.Common.UdonInputEventArgs",
            "InputLookHorizontal|floatValue:System.Single|args:VRC.Udon.Common.UdonInputEventArgs",
            "InputLookVertical|floatValue:System.Single|args:VRC.Udon.Common.UdonInputEventArgs",
            "InputMoveHorizontal|floatValue:System.Single|args:VRC.Udon.Common.UdonInputEventArgs",
            "InputMoveVertical|floatValue:System.Single|args:VRC.Udon.Common.UdonInputEventArgs",
            "InputUse|boolValue:System.Boolean|args:VRC.Udon.Common.UdonInputEventArgs",
            "OnAnimatorIK|layerIndex:System.Int32", "OnAnimatorMove", "OnBecameInvisible", "OnBecameVisible",
            "OnCollisionEnter|other:UnityEngine.Collision", "OnCollisionEnter2D|other:UnityEngine.Collision2D",
            "OnCollisionExit|other:UnityEngine.Collision", "OnCollisionExit2D|other:UnityEngine.Collision2D",
            "OnCollisionStay|other:UnityEngine.Collision", "OnCollisionStay2D|other:UnityEngine.Collision2D",
            "OnControllerColliderHit|hit:UnityEngine.ControllerColliderHit",
            "OnDestroy", "OnDisable", "OnEnable",
            "OnJointBreak|breakForce:System.Single", "OnJointBreak2D|brokenJoint:UnityEngine.Joint2D",
            "OnMouseDown", "OnMouseDrag", "OnMouseEnter", "OnMouseExit", "OnMouseOver", "OnMouseUp", "OnMouseUpAsButton",
            "OnParticleCollision|other:UnityEngine.GameObject", "OnParticleTrigger",
            "OnPostRender", "OnPreCull", "OnPreRender",
            "OnRenderImage|src:UnityEngine.RenderTexture|dest:UnityEngine.RenderTexture",
            "OnRenderObject", "OnTransformChildrenChanged", "OnTransformParentChanged",
            "OnTriggerEnter|other:UnityEngine.Collider", "OnTriggerEnter2D|other:UnityEngine.Collider2D",
            "OnTriggerExit|other:UnityEngine.Collider", "OnTriggerExit2D|other:UnityEngine.Collider2D",
            "OnTriggerStay|other:UnityEngine.Collider", "OnTriggerStay2D|other:UnityEngine.Collider2D",
            "OnWillRenderObject",
            "MidiControlChange|channel:System.Int32|number:System.Int32|value:System.Int32",
            "MidiNoteOff|channel:System.Int32|number:System.Int32|velocity:System.Int32",
            "MidiNoteOn|channel:System.Int32|number:System.Int32|velocity:System.Int32",
            "OnAsyncGpuReadbackComplete|request:VRC.SDK3.Rendering.VRCAsyncGPUReadbackRequest",
            "OnAvatarChanged|player:VRC.SDKBase.VRCPlayerApi",
            "OnAvatarEyeHeightChanged|player:VRC.SDKBase.VRCPlayerApi|prevEyeHeightAsMeters:System.Single",
            "OnContactEnter|contactInfo:VRC.Dynamics.ContactEnterInfo",
            "OnContactExit|contactInfo:VRC.Dynamics.ContactExitInfo",
            "OnControllerColliderHitPlayer|hit:VRC.SDK3.ControllerColliderPlayerHit",
            "OnDeserialization|result:VRC.Udon.Common.DeserializationResult",
            "OnDroneTriggerEnter|drone:VRC.SDKBase.VRCDroneApi",
            "OnDroneTriggerExit|drone:VRC.SDKBase.VRCDroneApi",
            "OnDroneTriggerStay|drone:VRC.SDKBase.VRCDroneApi",
            "OnDrop",
            "OnImageLoadError|IVRCImageDownload:VRC.SDK3.Image.IVRCImageDownload",
            "OnImageLoadSuccess|IVRCImageDownload:VRC.SDK3.Image.IVRCImageDownload",
            "OnInputMethodChanged|inputMethod:VRC.SDKBase.VRCInputMethod",
            "OnLanguageChanged|language:System.String",
            "OnListAvailableProducts|result:VRC.Economy.IProduct[]",
            "OnListProductOwners|result:VRC.Economy.IProduct|owners:System.String[]",
            "OnListPurchases|result:VRC.Economy.IProduct[]|player:VRC.SDKBase.VRCPlayerApi",
            "OnMasterTransferred|newMaster:VRC.SDKBase.VRCPlayerApi",
            "OnOwnershipRequest|requester:VRC.SDKBase.VRCPlayerApi|newOwner:VRC.SDKBase.VRCPlayerApi",
            "OnOwnershipTransferred|player:VRC.SDKBase.VRCPlayerApi",
            "OnPersistenceUsageUpdated",
            "OnPhysBoneGrabbed|physBoneInfo:VRC.Dynamics.PhysBoneGrabbedInfo",
            "OnPhysBonePosed|physBoneInfo:VRC.Dynamics.PhysBonePosedInfo",
            "OnPhysBoneReleased|physBoneInfo:VRC.Dynamics.PhysBoneReleasedInfo",
            "OnPhysBoneUnPosed|physBoneInfo:VRC.Dynamics.PhysBoneUnPosedInfo",
            "OnPickup", "OnPickupUseDown", "OnPickupUseUp",
            "OnPlayerCollisionEnter|player:VRC.SDKBase.VRCPlayerApi",
            "OnPlayerCollisionExit|player:VRC.SDKBase.VRCPlayerApi",
            "OnPlayerCollisionStay|player:VRC.SDKBase.VRCPlayerApi",
            "OnPlayerDataStorageExceeded|player:VRC.SDKBase.VRCPlayerApi",
            "OnPlayerDataStorageWarning|player:VRC.SDKBase.VRCPlayerApi",
            "OnPlayerDataUpdated|player:VRC.SDKBase.VRCPlayerApi|infos:VRC.SDK3.Persistence.PlayerData+Info[]",
            "OnPlayerJoined|player:VRC.SDKBase.VRCPlayerApi",
            "OnPlayerLeft|player:VRC.SDKBase.VRCPlayerApi",
            "OnPlayerObjectStorageExceeded|player:VRC.SDKBase.VRCPlayerApi",
            "OnPlayerObjectStorageWarning|player:VRC.SDKBase.VRCPlayerApi",
            "OnPlayerParticleCollision|player:VRC.SDKBase.VRCPlayerApi",
            "OnPlayerRespawn|player:VRC.SDKBase.VRCPlayerApi",
            "OnPlayerRestored|player:VRC.SDKBase.VRCPlayerApi",
            "OnPlayerSuspendChanged|player:VRC.SDKBase.VRCPlayerApi",
            "OnPlayerTriggerEnter|player:VRC.SDKBase.VRCPlayerApi",
            "OnPlayerTriggerExit|player:VRC.SDKBase.VRCPlayerApi",
            "OnPlayerTriggerStay|player:VRC.SDKBase.VRCPlayerApi",
            "OnPostSerialization|result:VRC.Udon.Common.SerializationResult",
            "OnPreSerialization",
            "OnProductEvent|result:VRC.Economy.IProduct|player:VRC.SDKBase.VRCPlayerApi",
            "OnPurchaseConfirmed|result:VRC.Economy.IProduct|player:VRC.SDKBase.VRCPlayerApi|purchasedNow:System.Boolean",
            "OnPurchaseConfirmedMultiple|result:VRC.Economy.IProduct|player:VRC.SDKBase.VRCPlayerApi|purchasedNow:System.Boolean|quantity:System.Int32",
            "OnPurchaseExpired|result:VRC.Economy.IProduct|player:VRC.SDKBase.VRCPlayerApi",
            "OnPurchasesLoaded|result:VRC.Economy.IProduct[]|player:VRC.SDKBase.VRCPlayerApi",
            "OnScreenUpdate|data:VRC.SDK3.Platform.ScreenUpdateData",
            "OnSpawn",
            "OnStationEntered|player:VRC.SDKBase.VRCPlayerApi",
            "OnStationExited|player:VRC.SDKBase.VRCPlayerApi",
            "OnStringLoadError|IVRCStringDownload:VRC.SDK3.StringLoading.IVRCStringDownload",
            "OnStringLoadSuccess|IVRCStringDownload:VRC.SDK3.StringLoading.IVRCStringDownload",
            "OnVRCCameraSettingsChanged|camera:VRC.SDK3.Rendering.VRCCameraSettings",
            "OnVRCPlusMassGift|gifter:VRC.SDKBase.VRCPlayerApi|numGifts:System.Int32",
            "OnVRCQualitySettingsChanged",
            "OnVideoEnd",
            "OnVideoError|videoError:VRC.SDK3.Components.Video.VideoError",
            "OnVideoLoop", "OnVideoReady", "OnVideoStart",
        };

    } // namespace

    struct Catalog::Impl {
        StringMap<std::unique_ptr<TypeInfo>>                types;
        StringMap<const TypeInfo*>                          byFullName;
        StringMap<std::vector<const TypeInfo*>>             byShortName;
        std::unordered_set<std::string, StringHash, std::equal_to<>> namespaces;
        StringMap<std::unique_ptr<ExternInfo>>              externs;
        StringMap<StringMap<std::vector<const ExternInfo*>>> methods;
        StringMap<std::unique_ptr<EventInfo>>               events;

        void Unindex(const TypeInfo& type) {
            if (type.fullName.empty()) return;
            byFullName.erase(type.fullName);
            auto it = byShortName.find(ShortName(type.fullName));
            if (it != byShortName.end()) std::erase(it->second, &type);
        }
    };

    Catalog::Catalog() : impl_(std::make_unique<Impl>()) {}
    Catalog::~Catalog() = default;
    Catalog::Catalog(Catalog&&) noexcept = default;
    Catalog& Catalog::operator=(Catalog&&) noexcept = default;

    void Catalog::AddType(TypeInfo type) {
        if (type.udonName.empty()) type.udonName = UdonTypeName(type.fullName);
        if (type.udonName.empty()) return;

        auto& slot = impl_->types[type.udonName];
        if (slot) impl_->Unindex(*slot);
        slot = std::make_unique<TypeInfo>(std::move(type));
        const TypeInfo* stored = slot.get();

        if (stored->fullName.empty()) return;
        impl_->byFullName[stored->fullName] = stored;
        impl_->byShortName[std::string(ShortName(stored->fullName))].push_back(stored);

        std::string_view path = stored->fullName;
        for (size_t dot = path.find('.'); dot != std::string_view::npos; dot = path.find('.', dot + 1))
            impl_->namespaces.emplace(path.substr(0, dot));
    }

    bool Catalog::AddExtern(std::string_view signature, int parameterCount) {
        ExternInfo info;
        if (!ParseExternSignature(signature, info)) return false;

        int declared = static_cast<int>(info.parameters.size()) + (info.returnType == "SystemVoid" ? 0 : 1) + (info.isGeneric ? 1 : 0);
        int receiver = parameterCount - declared;
        if (receiver != 0 && receiver != 1) return false;
        info.isStatic = receiver == 0 || info.isConstructor;
        if (info.isConstructor && receiver != 0) return false;
        info.parameterCount = parameterCount;

        auto& slot = impl_->externs[info.signature];
        if (slot) std::erase(impl_->methods[slot->module][slot->method], slot.get());
        slot = std::make_unique<ExternInfo>(std::move(info));
        impl_->methods[slot->module][slot->method].push_back(slot.get());
        return true;
    }

    void Catalog::AddEvent(EventInfo event) {
        if (event.name.empty()) return;
        std::string key = event.name;
        impl_->events[key] = std::make_unique<EventInfo>(std::move(event));
    }

    void Catalog::AddStandardEvents() {
        for (std::string_view row : kStandardEvents) {
            EventInfo event;
            size_t bar = row.find('|');
            event.name = std::string(row.substr(0, bar));
            while (bar != std::string_view::npos) {
                size_t next = row.find('|', bar + 1);
                std::string_view field = row.substr(bar + 1, next == std::string_view::npos ? std::string_view::npos : next - bar - 1);
                size_t colon = field.find(':');
                event.parameters.push_back({ std::string(field.substr(0, colon)), UdonTypeName(field.substr(colon + 1)) });
                bar = next;
            }
            if (!FindEvent(event.name)) AddEvent(std::move(event));
        }
    }

    const TypeInfo* Catalog::FindType(std::string_view udonName) const {
        auto it = impl_->types.find(udonName);
        return it == impl_->types.end() ? nullptr : it->second.get();
    }

    const TypeInfo* Catalog::FindTypeByFullName(std::string_view fullName) const {
        auto it = impl_->byFullName.find(fullName);
        return it == impl_->byFullName.end() ? nullptr : it->second;
    }

    std::vector<const TypeInfo*> Catalog::FindTypesByShortName(std::string_view shortName) const {
        auto it = impl_->byShortName.find(shortName);
        return it == impl_->byShortName.end() ? std::vector<const TypeInfo*>{} : it->second;
    }

    bool Catalog::IsNamespace(std::string_view path) const {
        return impl_->namespaces.find(path) != impl_->namespaces.end();
    }

    const ExternInfo* Catalog::FindExtern(std::string_view signature) const {
        auto it = impl_->externs.find(signature);
        return it == impl_->externs.end() ? nullptr : it->second.get();
    }

    std::span<const ExternInfo* const> Catalog::FindMethods(std::string_view module, std::string_view method) const {
        auto m = impl_->methods.find(module);
        if (m == impl_->methods.end()) return {};
        auto it = m->second.find(method);
        if (it == m->second.end()) return {};
        return it->second;
    }

    const EventInfo* Catalog::FindEvent(std::string_view name) const {
        auto it = impl_->events.find(name);
        return it == impl_->events.end() ? nullptr : it->second.get();
    }

    std::vector<const TypeInfo*> Catalog::Types() const {
        std::vector<const TypeInfo*> out;
        out.reserve(impl_->types.size());
        for (auto& [_, t] : impl_->types) out.push_back(t.get());
        return out;
    }

    std::vector<const ExternInfo*> Catalog::Externs() const {
        std::vector<const ExternInfo*> out;
        out.reserve(impl_->externs.size());
        for (auto& [_, e] : impl_->externs) out.push_back(e.get());
        return out;
    }

    std::vector<const EventInfo*> Catalog::Events() const {
        std::vector<const EventInfo*> out;
        out.reserve(impl_->events.size());
        for (auto& [_, e] : impl_->events) out.push_back(e.get());
        return out;
    }

    bool ParseExternSignature(std::string_view signature, ExternInfo& out) {
        size_t dot = signature.find('.');
        if (dot == std::string_view::npos || dot == 0) return false;
        std::string_view rest = signature.substr(dot + 1);
        if (!rest.starts_with("__")) return false;
        rest.remove_prefix(2);

        size_t nameEnd = rest.find("__");
        if (nameEnd == std::string_view::npos || nameEnd == 0) return false;
        std::string_view method = rest.substr(0, nameEnd);
        rest.remove_prefix(nameEnd + 2);

        std::string_view params;
        std::string_view ret;
        size_t split = rest.rfind("__");
        if (split == std::string_view::npos) {
            ret = rest;
        } else {
            params = rest.substr(0, split);
            ret = rest.substr(split + 2);
        }
        if (ret.empty()) return false;

        out = {};
        out.signature = std::string(signature);
        out.module = std::string(signature.substr(0, dot));
        out.method = std::string(method);
        out.parameterText = std::string(params);
        out.parameters = SplitParameters(params);
        out.returnType = std::string(ret);
        if (method.starts_with("set_") && out.parameters.empty() && ret != "SystemVoid") {
            out.parameters.push_back(out.returnType);
            out.parameterText = out.returnType;
            out.returnType = "SystemVoid";
        }
        out.isConstructor = method == "ctor";
        out.isGeneric = IsGenericName(ret) || std::ranges::any_of(out.parameters, [](const std::string& p) { return IsGenericName(p); });
        return std::ranges::none_of(out.parameters, [](const std::string& p) { return p.empty(); });
    }

    std::string UdonTypeName(std::string_view fullName) {
        std::string out;
        out.reserve(fullName.size() + 8);
        for (size_t i = 0; i < fullName.size(); ++i) {
            char c = fullName[i];
            if (c == '.' || c == '+' || c == ',') continue;
            if (c == '[' && i + 1 < fullName.size() && fullName[i + 1] == ']') {
                out += "Array";
                ++i;
                continue;
            }
            if (c == '&') {
                out += "Ref";
                continue;
            }
            out += c;
        }
        return out;
    }

} // namespace UdonLuau
