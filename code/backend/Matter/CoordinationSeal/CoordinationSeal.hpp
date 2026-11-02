#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace singularity::matter::coordination::seal {

/*
 * Every byte one of the firm's computers sends another about a Matter is
 * sealed here, on the computer that sends it, and opened only on the one it is
 * for (D-123). Whatever carries it - the firm's own folder, a direct link
 * (Matter/CoordinationTransport, D-124) - sees opaque bytes and their length.
 *
 * Keys:
 * - each shared Matter has its own key, made by the computer that shares it;
 * - each computer has its own X25519 key pair for coordination, whose public
 *   half it announces beside its Ed25519 device key (Platform/DeviceKey);
 * - a Matter's key reaches a computer only sealed to that computer's own key,
 *   by a computer of the Matter, once the lawyers have compared the joining
 *   computer's code (D-095).
 *
 * Envelopes, by their first byte:
 * - 0x01, a change: sealed under the Matter's key; the engine's bytes inside.
 * - 0x02, a Matter's key, sealed to one computer's key, signed by the sender.
 * - 0x03, a computer asking to join a Matter: its public keys, signed by its
 *   device key. Nothing in it is secret.
 * - 0x04, where a computer can be reached: sealed under the Matter's key,
 *   signed by its device key, with the time it expires.
 *
 * Everything is XChaCha20-Poly1305 under keys derived by keyed BLAKE2b
 * (Monocypher 4), with a fresh 24-byte random nonce for every seal, bound as
 * authenticated data to what routes it, and padded (Padmé) so its length says
 * little. A change carries its sender's counter, always rising, so an old one
 * sent again is recognised (the engine keeps what it has seen).
 */

constexpr std::size_t kKeyBytes = 32;

enum class Kind : std::uint8_t { Unknown = 0, Change = 1, KeyWrap = 2, JoinRequest = 3, AddressCard = 4, Inventory = 5, AccountGift = 6 };

// What an envelope is, by its first byte.
Kind kind_of(const std::string &envelope);

// An envelope's id, by which every carrier and the engine recognise it, is
// Matter/CoordinationTransport's envelope_id (D-124).

// ---- Keys -------------------------------------------------------------------

// A Matter's key. Its bytes are wiped when it goes.
struct MatterKey {
	std::array<std::uint8_t, kKeyBytes> bytes {};
	MatterKey() = default;
	MatterKey(const MatterKey &) = default;
	MatterKey &operator=(const MatterKey &) = default;
	~MatterKey();
};

MatterKey make_matter_key();

// The key's id: what an envelope names to say which key opens it, and what
// tells an old key from its successor. Sixteen hex characters; it tells
// nothing of the key.
std::string matter_key_id(const MatterKey &key);

// This computer's own key pair for coordination. The secret half never leaves
// the computer's credential store.
struct ComputerKeys {
	std::array<std::uint8_t, kKeyBytes> secret {};
	std::array<std::uint8_t, kKeyBytes> public_key {};
	ComputerKeys() = default;
	ComputerKeys(const ComputerKeys &) = default;
	ComputerKeys &operator=(const ComputerKeys &) = default;
	~ComputerKeys();
};

ComputerKeys make_computer_keys();
// The pair from its secret half, as kept.
ComputerKeys computer_keys_from_secret(const std::array<std::uint8_t, kKeyBytes> &secret);

// Keys as text for this computer's own records: base64 of the bytes.
std::string key_text(const std::array<std::uint8_t, kKeyBytes> &bytes);
std::optional<std::array<std::uint8_t, kKeyBytes>> key_from_text(const std::string &text);

// The device key's signature, as Platform/DeviceKey makes and checks it
// (Ed25519, base64): given to the seal so it stays free of this computer's
// files, and a test can sign with keys of its own.
using Sign = std::function<std::string(const std::string &message)>;
using Verify = std::function<bool(const std::string &device_public_key, const std::string &message,
	const std::string &signature)>;

// ---- 0x01: a change, under the Matter's key ---------------------------------

// What routes an envelope: which Matter, from which computer, for which
// (empty: every computer of the Matter), and the sender's counter. All of it
// is bound to the seal; the carrier may read it, the content it may not.
struct Header {
	std::string matter_id;
	std::string from_device;
	std::vector<std::string> to_devices;
	std::int64_t counter = 0;
};

std::string seal_change(const MatterKey &key, const Header &header, const std::string &plaintext);

struct OpenedChange {
	Header header;
	std::string key_id;
	std::string plaintext;
};

// What an envelope says of itself before it is opened: its kind, the key it
// names and its header. None for what is not an envelope of this format.
struct Outside {
	Kind kind = Kind::Unknown;
	std::string key_id;
	Header header;
};
std::optional<Outside> outside_of(const std::string &envelope);

// The change, or none when it is not sealed with this key, or any byte of it
// was altered.
std::optional<OpenedChange> open_change(const MatterKey &key, const std::string &envelope);

// ---- 0x02: a Matter's key, sealed to one computer ---------------------------

struct KeyGift {
	std::string matter_id;
	std::string from_device;
	// The sender's device key (Ed25519, base64): what the receiving lawyer
	// compares, as its code, before the key is taken (D-095, D-123).
	std::string from_device_key;
	std::string to_device;
	MatterKey key;
	std::int64_t counter = 0;
	// What the Matter held when its key was given - its files and their bytes
	// -, so the lawyer taking it knows how much is coming before any of it is
	// opened (D-207). In the header the signature and the seal cover; zero when
	// not said.
	std::uint64_t files = 0;
	std::uint64_t bytes = 0;
};

// Sealed to `recipient_public` (X25519) with a key made for this seal alone,
// and signed by the sender's device key.
std::string seal_key(const KeyGift &gift, const std::array<std::uint8_t, kKeyBytes> &recipient_public, const Sign &sign);

// The gift, opened with this computer's keys; none unless it was sealed to
// them, names `sender_device_key` as its sender's, and is signed by it.
std::optional<KeyGift> open_key(const std::string &envelope, const ComputerKeys &mine,
	const std::string &sender_device_key, const Verify &verify);

// Who a key envelope says it is from and for, before it is opened.
struct KeyOutside {
	std::string matter_id;
	std::string from_device;
	std::string from_device_key;
	std::string to_device;
	// The Matter's size as its giver said it (D-207); zero when not said.
	std::uint64_t files = 0;
	std::uint64_t bytes = 0;
};
std::optional<KeyOutside> key_outside(const std::string &envelope);

// ---- 0x06: one of the firm's model accounts, given (D-210) --------------------

// A model account connected on one of the firm's computers, given to another
// that its lawyer checked: the account as that computer's settings keep it and
// its key, sealed to the receiving computer's own X25519 key with a key pair
// made for the seal alone, and signed by the giver's device key. It travels
// as sealed bytes only and is opened straight into the receiving computer's
// credential store (AG-SEC-1, AG-SEC-9). `removed`: the account is no longer
// the firm's to use there.
struct AccountGift {
	// The firm's record's channel, which routes it.
	std::string matter_id;
	std::string from_device;
	std::string from_device_key;
	std::string to_device;
	std::int64_t counter = 0;
	std::string provider_id;
	// The account as settings.json writes it (Platform/AppSettings), no key in it.
	std::string entry;
	std::string credential;
	bool removed = false;
	AccountGift() = default;
	AccountGift(const AccountGift &) = default;
	AccountGift &operator=(const AccountGift &) = default;
	// Its key's bytes are wiped when it goes.
	~AccountGift();
};

std::string seal_account(const AccountGift &gift, const std::array<std::uint8_t, kKeyBytes> &recipient_public, const Sign &sign);
// The gift, opened with this computer's keys; none unless it was sealed to
// them, names `sender_device_key` as its sender's, and is signed by it.
std::optional<AccountGift> open_account(const std::string &envelope, const ComputerKeys &mine,
	const std::string &sender_device_key, const Verify &verify);
// Who an account envelope says it is from and for, before it is opened.
struct AccountOutside {
	std::string matter_id;
	std::string from_device;
	std::string from_device_key;
	std::string to_device;
	std::string provider_id;
};
std::optional<AccountOutside> account_outside(const std::string &envelope);

// ---- 0x03: a computer asks to join ------------------------------------------

struct JoinRequest {
	std::string matter_id;
	std::string device_id;
	// The member whose computer it is (an actor id): whom the lawyers confirm it for.
	std::string actor_id;
	// Ed25519, base64 (Platform/DeviceKey): what the lawyers' code is made from.
	std::string device_key;
	std::array<std::uint8_t, kKeyBytes> coordination_key {};
	std::string at;
};

std::string seal_join_request(const JoinRequest &request, const Sign &sign);
// The request, if it is signed by the device key it names.
std::optional<JoinRequest> open_join_request(const std::string &envelope, const Verify &verify);

// ---- 0x04: where a computer can be reached ----------------------------------

struct AddressCard {
	std::string matter_id;
	std::string device_id;
	// Where to reach it ("udp:192.168.1.20:41641", "udp:203.0.113.7:41641"),
	// for the carriers to read (D-124).
	std::vector<std::string> candidates;
	// Its X25519 key (key_text), which its direct link's handshake proves
	// (D-124): signed with the rest, so it is that device's.
	std::string coordination_key;
	std::string expires_at;
	std::int64_t counter = 0;
};

std::string seal_address_card(const MatterKey &key, const AddressCard &card, const Sign &sign);
std::optional<AddressCard> open_address_card(const MatterKey &key, const std::string &envelope,
	const std::string &device_key, const Verify &verify);

// ---- 0x05: a Matter whole, as one computer sends it -------------------------

// One file of a Matter as the sending computer holds it: its place in the
// Matter's folder (forward slashes), its SHA-256 in lowercase hex, its size,
// and the change it is - the counter and the computer of the change that wrote
// it there (0 and empty when none is known).
struct InventoryEntry {
	std::string path;
	std::string sha256;
	std::uint64_t size = 0;
	std::int64_t counter = 0;
	std::string sender;
};

// Every file of a Matter that travels, as one computer holds it, told to
// another of its computers: when they meet after a time apart, so each sends
// the other its own changes the other does not hold; and first, when it sends
// a Matter whole, so the computer it reaches knows when all of it is there.
// The names are the Matter's, so it is sealed under the Matter's key like a
// change; a carrier sees only its header and length.
struct Inventory {
	Header header;
	std::vector<InventoryEntry> files;
};

// Throws std::invalid_argument for an entry with no path, a path that leaves
// the folder, or a hash that is not SHA-256.
std::string seal_inventory(const MatterKey &key, const Inventory &inventory);
// None when it is not sealed with this key, any byte was altered, or any entry
// is not a file of the folder with a SHA-256.
std::optional<Inventory> open_inventory(const MatterKey &key, const std::string &envelope);

// ---- Names ------------------------------------------------------------------

// A name kept for others to see only with the Matter's key - a computer's, a
// Matter's - sealed and bound to what it names.
std::string seal_name(const MatterKey &key, const std::string &kind, const std::string &id, const std::string &name);
std::optional<std::string> open_name(const MatterKey &key, const std::string &kind, const std::string &id,
	const std::string &sealed);

// How long a change of `plaintext_bytes` is once sealed, at most.
std::size_t sealed_size_at_most(std::size_t plaintext_bytes);

} // namespace singularity::matter::coordination::seal
