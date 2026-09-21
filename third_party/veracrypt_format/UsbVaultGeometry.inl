// Included only by the separately distributed VeraCrypt FormatDLL.
// This boundary consumes encrypted headers, never paths, and exports no keys.
namespace {
struct UsbVaultHeaderState
{
	Password password;
	unsigned char encrypted[TC_VOLUME_HEADER_EFFECTIVE_SIZE];
	PCRYPTO_INFO crypto;
	UsbVaultHeaderState() : password(), encrypted(), crypto(NULL) {}
	~UsbVaultHeaderState()
	{
		if (crypto) crypto_close(crypto);
		burn(&password, sizeof(password));
		burn(encrypted, sizeof(encrypted));
	}
	UsbVaultHeaderState(const UsbVaultHeaderState&) = delete;
	UsbVaultHeaderState& operator=(const UsbVaultHeaderState&) = delete;
};

// Dedicated locked pages prevent unlocks of unrelated stack-resident secrets.
class UsbVaultLockedHeaders
{
public:
	UsbVaultHeaderState* states;
	UsbVaultLockedHeaders() : states(NULL)
	{
		void* memory = VirtualAlloc(NULL, 2 * sizeof(UsbVaultHeaderState),
			MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!memory) return;
		if (!VirtualLock(memory, 2 * sizeof(UsbVaultHeaderState)))
		{
			VirtualFree(memory, 0, MEM_RELEASE);
			return;
		}
		states = static_cast<UsbVaultHeaderState*>(memory);
		new (&states[0]) UsbVaultHeaderState();
		new (&states[1]) UsbVaultHeaderState();
	}
	~UsbVaultLockedHeaders()
	{
		if (!states) return;
		states[1].~UsbVaultHeaderState();
		states[0].~UsbVaultHeaderState();
		burn(states, 2 * sizeof(UsbVaultHeaderState));
		VirtualUnlock(states, 2 * sizeof(UsbVaultHeaderState));
		VirtualFree(states, 0, MEM_RELEASE);
	}
	UsbVaultLockedHeaders(const UsbVaultLockedHeaders&) = delete;
	UsbVaultLockedHeaders& operator=(const UsbVaultLockedHeaders&) = delete;
};

bool UsbVaultReadGeometryHeader(UsbVaultHeaderState& state,
	const unsigned char* header, const unsigned char* password, uint32_t length)
{
	memcpy(state.encrypted, header, sizeof(state.encrypted));
	memcpy(state.password.Text, password, length);
	state.password.Length = length;
	return ReadVolumeHeader(FALSE, state.encrypted, &state.password,
		SHA512, 0, &state.crypto, NULL) == ERR_SUCCESS && state.crypto;
}

bool UsbVaultGeometryHeaderSupported(const CRYPTO_INFO* info)
{
	// Match the current product's non-system file-container format, explicitly.
	// Do not guess geometry for legacy, partially encrypted or flagged layouts.
	return info->HeaderVersion == 5 && info->HeaderFlags == 0
		&& info->SectorSize == TC_SECTOR_SIZE_FILE_HOSTED_VOLUME
		&& info->VolumeSize.Value != 0
		&& info->EncryptedAreaLength.Value == info->VolumeSize.Value
		&& info->EncryptedAreaStart.Value % ENCRYPTION_DATA_UNIT_SIZE == 0
		&& info->VolumeSize.Value % ENCRYPTION_DATA_UNIT_SIZE == 0;
}
}

// Fixed ABI: four uint64 values, offsets relative to the container file.
// Both primary headers must come from the same caller-held file handle.
extern "C" VCF_API int __cdecl VeraCryptReadHiddenGeometry(
	const unsigned char* outerHeader, uint32_t outerHeaderBytes,
	const unsigned char* hiddenHeader, uint32_t hiddenHeaderBytes,
	const unsigned char* outerPassword, uint32_t outerPasswordBytes,
	const unsigned char* hiddenPassword, uint32_t hiddenPasswordBytes,
	uint64_t hostBytes, uint64_t* geometry, uint32_t geometryBytes)
{
	if (!geometry || geometryBytes != 4 * sizeof(uint64_t))
		return VCF_ERROR_INVALID_PARAMETER;
	memset(geometry, 0, geometryBytes);
	if (!outerHeader || !hiddenHeader || !outerPassword || !hiddenPassword
		|| outerHeaderBytes != TC_VOLUME_HEADER_EFFECTIVE_SIZE
		|| hiddenHeaderBytes != TC_VOLUME_HEADER_EFFECTIVE_SIZE
		|| outerPasswordBytes == 0 || outerPasswordBytes > MAX_PASSWORD
		|| hiddenPasswordBytes == 0 || hiddenPasswordBytes > MAX_PASSWORD
		|| hostBytes <= TC_TOTAL_VOLUME_HEADERS_SIZE
		|| hostBytes > INT64_MAX || hostBytes % ENCRYPTION_DATA_UNIT_SIZE != 0)
		return VCF_ERROR_INVALID_PARAMETER;
	try
	{
		std::lock_guard<std::mutex> lock(g_sdkMutex);
		if (!g_isInitialized) return VCF_ERROR_NOT_INITIALIZED;
		UsbVaultLockedHeaders headers;
		if (!headers.states) return VCF_ERROR_OUT_OF_MEMORY;
		// Evaluate both headers even when one password is wrong. No PRF guessing.
		const bool outerOk = UsbVaultReadGeometryHeader(headers.states[0],
			outerHeader, outerPassword, outerPasswordBytes);
		const bool hiddenOk = UsbVaultReadGeometryHeader(headers.states[1],
			hiddenHeader, hiddenPassword, hiddenPasswordBytes);
		if (!outerOk || !hiddenOk) return VCF_ERROR_GENERIC;
		const CRYPTO_INFO* outer = headers.states[0].crypto;
		const CRYPTO_INFO* hidden = headers.states[1].crypto;
		if (!UsbVaultGeometryHeaderSupported(outer)
			|| !UsbVaultGeometryHeaderSupported(hidden)
			|| outer->hiddenVolume || !hidden->hiddenVolume
			|| hidden->hiddenVolumeSize != hidden->VolumeSize.Value
			|| outer->EncryptedAreaStart.Value != TC_VOLUME_DATA_OFFSET
			|| outer->VolumeSize.Value != hostBytes - TC_TOTAL_VOLUME_HEADERS_SIZE)
			return VCF_ERROR_GENERIC;
		const uint64_t outerEnd = hostBytes - TC_VOLUME_HEADER_GROUP_SIZE;
		const uint64_t start = hidden->EncryptedAreaStart.Value;
		const uint64_t length = hidden->hiddenVolumeSize;
		// Subtraction-based bounds avoid attacker-controlled addition overflow.
		if (start < outer->EncryptedAreaStart.Value || start >= outerEnd
			|| length > outerEnd - start)
			return VCF_ERROR_GENERIC;
		geometry[0] = outer->EncryptedAreaStart.Value;
		geometry[1] = outer->VolumeSize.Value;
		geometry[2] = start;
		geometry[3] = length;
		return VCF_SUCCESS;
	}
	catch (...) { return VCF_ERROR_GENERIC; }
}
