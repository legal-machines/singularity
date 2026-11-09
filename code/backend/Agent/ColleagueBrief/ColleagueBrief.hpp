#pragma once

#include <string>

namespace singularity::agents {

/*
 * What every request of an AI colleague carries before its work: the firm's
 * one way of professional communication, the same for every colleague (layer
 * 1), and this colleague's role - its standing work, its manner, whom it
 * answers to and what it may do here (layer 2). The Matter, its thread and
 * its documents (layer 3), and what actually happened when the work was done
 * (layer 4), come with each request, never here.
 *
 * Every colleague's request is built from this one brief - a thread's answer,
 * a review, a hand-in - so a colleague sounds like itself everywhere and no
 * second way of instructing a model grows beside it.
 */

// Layer 1: how every AI colleague of the firm communicates.
const std::string &professional_communication();

struct ColleagueRole {
	std::string name;
	// The firm it is a member of, as its people name it; may be empty. A
	// colleague is a member of the whole firm, never of one Matter: the Matter
	// of a piece of work comes with that work.
	std::string firm;
	// Its office in the firm (Agent::office): the office it holds, the position
	// of the firm's scale it ranks with, its practice and its area; each may be
	// empty.
	std::string office;
	std::string ranks_with;
	std::string practice;
	std::string area;
	// Its standing work, in the words of the lawyer who set it.
	std::string standing_work;
	// Its professional habits, in a sentence: what makes it this colleague and
	// not another (Kitsune: wording and contradictions; Unicorn: the deal's
	// commercial side and the next practical step). Empty: none of its own.
	std::string manner;
	// Whether it works only with models on this computer.
	bool works_only_here = false;
	// Who it works with in the firm, as the firm names them, one entry each in
	// the office they hold ("Dana Whitfield (Partner, Commercial)"), sorted, so
	// its brief is the same bytes until the team changes; may be empty.
	std::string team;
};

// Layers 1 and 2, as the request's standing instructions.
std::string colleague_brief(const ColleagueRole &role);

// Who the colleague is in the firm, as its talk carries it: its name, its
// office, rank, practice and area, its manner, whom it answers to, and what
// it can do and never does - layer 2 only, the firm's long rules travelling
// with its work. Where it holds no office, its standing work says what it is
// there for.
std::string colleague_identity(const ColleagueRole &role);

} // namespace singularity::agents
