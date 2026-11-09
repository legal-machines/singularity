#pragma once

#include "Matter/CoordinationTransport/CoordinationTransport.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace singularity::matter::coordination::transport {

// The firm's own folder as a transport: any folder its computers keep in step
// with the firm's own storage - OneDrive or SharePoint, Dropbox, Google Drive,
// iCloud Drive, a NAS. Nothing passes through Legal Machines, and the
// storage's owner sees only names that mean nothing, sizes the seal pads, and
// times.
// - A Matter's envelopes stand in a folder of their own, named by a hash of
//   the Matter's id, never by the id or the Matter's name.
// - Each envelope is one file named by its id ("<id>.lme"): written whole
//   under a passing name, then renamed into place; sent again, nothing
//   changes.
// - A file whose bytes are not its id - still syncing, cut short, or put there
//   by someone else - is never offered, only looked at again later.
// - What this computer has taken is kept in its own records, never in the
//   shared folder.
class FolderTransport final : public Transport {
public:
	// `folder`: the exchange folder inside the firm's synced storage.
	// `records`: this computer's own folder for what it has taken.
	FolderTransport(std::filesystem::path folder, std::filesystem::path records);

	void send(const std::string& matterId, const SealedBytes& bytes, const std::vector<std::string>& toDevices) override;
	std::vector<SealedBytes> poll(const std::string& matterId) override;
	void ack(const std::string& matterId, const std::string& envelopeId) override;
	std::string name() const override;
	bool reaches_every_computer() const override { return true; }

	// Takes away the Matter's envelopes older than `age`: a computer away
	// longer than that is sent the Matter whole by one that has it. Returns
	// how many files were removed.
	std::size_t remove_older_than(const std::string& matterId, std::chrono::hours age);

	// The folder a Matter's envelopes stand in.
	std::filesystem::path matter_folder(const std::string& matterId) const;

	// Most envelopes and bytes one poll reads: a folder flooded by someone with
	// access to the storage slows nothing down.
	static constexpr std::size_t kMostPerPoll = 256;
	static constexpr std::size_t kMostBytesPerPoll = 256u * 1024u * 1024u;

private:
	std::filesystem::path folder_;
	std::filesystem::path records_;

	// What was taken: one envelope id a line, each added as it is taken, so a
	// Matter of thousands of envelopes is taken in time that grows with them,
	// not with their square. Written anew only to forget what the folder no
	// longer holds.
	std::filesystem::path takenFile(const std::string& matterId) const;
	std::set<std::string> taken(const std::string& matterId) const;
	void addTaken(const std::string& matterId, const std::string& envelopeId) const;
	void keepTaken(const std::string& matterId, const std::set<std::string>& ids) const;
	// The record as the first builds kept it, one JSON file, carried over once.
	void carryOverJsonRecord(const std::string& matterId) const;
};

} // namespace singularity::matter::coordination::transport
