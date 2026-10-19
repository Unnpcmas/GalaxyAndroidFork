#include "Game/Map/FileSelectFunc.hpp"
#include "Game/Map/FileSelectIconID.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/MessageUtil.hpp"
#include <RVLFaceLib.h>

namespace {
    /// @brief The array of message identifiers corresponding to File Selection Screen icon names.
    static const char* sIconNameMessageID[] = {
        "System_FileSelect_Icon000", "System_FileSelect_Icon001", "System_FileSelect_Icon002",
        "System_FileSelect_Icon003", "System_FileSelect_Icon004",
    };
};  // namespace

namespace FileSelectFunc {
    u32 getMiiNameBufferSize() {
        return RFL_NAME_LEN + 1;
    }

    void copyMiiName(u16* pName, const FileSelectIconID& rIcon) {
        if (pName == nullptr) {
            return;
        }

        const u32 nameBytes = getMiiNameBufferSize() * sizeof(wchar_t);
        MR::zeroMemory(pName, nameBytes);

        if (rIcon.isFellow()) {
            const u32 fellowId = static_cast<u32>(rIcon.getFellowID());
            if (fellowId >= sizeof(::sIconNameMessageID) / sizeof(::sIconNameMessageID[0])) {
                return;
            }

            const char* pMessageId = ::sIconNameMessageID[fellowId];
            const wchar_t* pMessage = MR::getGameMessageDirect(pMessageId);

            if (pMessage != nullptr) {
                MR::copyMemory(pName, pMessage, nameBytes);
            }
        } else if (rIcon.isMii()) {
            RFLAdditionalInfo info;
            RFLErrcode err = RFLGetAdditionalInfo(&info, RFLDataSource_Official, nullptr, rIcon.getMiiIndex());

            if (err == RFLErrcode_Success) {
                // The face library stores exactly RFL_NAME_LEN code units and
                // does not guarantee an extra terminator after that array.
                MR::copyMemory(pName, info.name, RFL_NAME_LEN * sizeof(wchar_t));
            }
        }
    }
};  // namespace FileSelectFunc
