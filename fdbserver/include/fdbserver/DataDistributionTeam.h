/*
 * DataDistributionTeam.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "fdbclient/StorageServerInterface.h"

struct IDataDistributionTeam {
	virtual std::vector<StorageServerInterface> getLastKnownServerInterfaces() const = 0;
	virtual int size() const = 0;
	virtual std::vector<UID> const& getServerIDs() const = 0;
	virtual void addDataInFlightToTeam(int64_t delta) = 0;
	virtual void addReadInFlightToTeam(int64_t delta) = 0;
	virtual int64_t getDataInFlightToTeam() const = 0;
	virtual int64_t getLoadBytes(bool includeInFlight = true, double inflightPenalty = 1.0) const = 0;
	virtual int64_t getReadInFlightToTeam() const = 0;
	virtual double getLoadReadBandwidth(bool includeInFlight = true, double inflightPenalty = 1.0) const = 0;
	virtual int64_t getMinAvailableSpace(bool includeInFlight = true) const = 0;
	virtual double getMinAvailableSpaceRatio(bool includeInFlight = true) const = 0;
	virtual bool hasHealthyAvailableSpace(double minRatio) const = 0;
	virtual Future<Void> updateStorageMetrics() = 0;
	virtual void addref() const = 0;
	virtual void delref() const = 0;
	virtual bool isHealthy() const = 0;
	virtual void setHealthy(bool) = 0;
	virtual int getPriority() const = 0;
	virtual void setPriority(int) = 0;
	virtual bool isOptimal() const = 0;
	virtual bool isWrongConfiguration() const = 0;
	virtual void setWrongConfiguration(bool) = 0;
	virtual void addServers(const std::vector<UID>& servers) = 0;
	virtual std::string getTeamID() const = 0;

	std::string getDesc() const {
		const auto& servers = getLastKnownServerInterfaces();
		std::string s = format("TeamID %s; ", getTeamID().c_str());
		s += format("Size %d; ", servers.size());
		for (int i = 0; i < servers.size(); i++) {
			if (i)
				s += ", ";
			s += servers[i].address().toString() + " " + servers[i].id().shortString();
		}
		return s;
	}
};

FDB_BOOLEAN_PARAM(WantNewServers);
FDB_BOOLEAN_PARAM(WantTrueBest);
FDB_BOOLEAN_PARAM(PreferLowerDiskUtil);
FDB_BOOLEAN_PARAM(TeamMustHaveShards);
FDB_BOOLEAN_PARAM(ForReadBalance);
FDB_BOOLEAN_PARAM(PreferLowerReadUtil);
FDB_BOOLEAN_PARAM(FindTeamByServers);

class TeamSelect {
public:
	enum Value : int8_t {
		ANY = 0, // Any other situations except for the next two
		WANT_COMPLETE_SRCS, // Try best to select a healthy team consists of servers in completeSources
		WANT_TRUE_BEST, // Ask for the most or least utilized team in the cluster
	};
	TeamSelect() : value(ANY) {}
	TeamSelect(Value v) : value(v) {}
	std::string toString() const {
		switch (value) {
		case WANT_COMPLETE_SRCS:
			return "Want_Complete_Srcs";
		case WANT_TRUE_BEST:
			return "Want_True_Best";
		case ANY:
			return "Any";
		}
	}

	bool operator==(const TeamSelect& tmpTeamSelect) { return value == tmpTeamSelect.value; }

private:
	Value value;
};

struct GetTeamRequest {
	TeamSelect teamSelect;
	bool preferLowerDiskUtil; // if true, lower utilized team has higher score
	bool teamMustHaveShards;
	bool forReadBalance;
	bool preferLowerReadUtil; // only make sense when forReadBalance is true
	double inflightPenalty;
	bool findTeamByServers;
	Optional<KeyRange> keys;

	// completeSources have all shards in the key range being considered for movement, src have at least 1 shard in the
	// key range for movement. From the point of set, completeSources is the Intersection set of several <server_lists>,
	// while src is the Union set of them. E.g. keyRange = [Shard_1, Shard_2), and Shard_1 is located at {Server_1,
	// Server_2, Server_3}, Shard_2 is located at {Server_2, Server_3, Server_4}. completeSources = {Server_2,
	// Server_3}, src = {Server_1, Server_2, Server_3, Server_4}
	std::vector<UID> completeSources;
	std::vector<UID> src;

	Promise<std::pair<Optional<Reference<IDataDistributionTeam>>, bool>> reply;

	typedef Reference<IDataDistributionTeam> TeamRef;

	GetTeamRequest() {}
	GetTeamRequest(TeamSelect teamSelectRequest,
	               PreferLowerDiskUtil preferLowerDiskUtil,
	               TeamMustHaveShards teamMustHaveShards,
	               ForReadBalance forReadBalance = ForReadBalance::False,
	               PreferLowerReadUtil preferLowerReadUtil = PreferLowerReadUtil::False,
	               double inflightPenalty = 1.0,
	               Optional<KeyRange> keys = Optional<KeyRange>())
	  : teamSelect(teamSelectRequest), preferLowerDiskUtil(preferLowerDiskUtil), teamMustHaveShards(teamMustHaveShards),
	    forReadBalance(forReadBalance), preferLowerReadUtil(preferLowerReadUtil), inflightPenalty(inflightPenalty),
	    findTeamByServers(FindTeamByServers::False), keys(keys) {}
	GetTeamRequest(std::vector<UID> servers)
	  : teamSelect(TeamSelect::WANT_COMPLETE_SRCS), preferLowerDiskUtil(PreferLowerDiskUtil::False),
	    teamMustHaveShards(TeamMustHaveShards::False), forReadBalance(ForReadBalance::False),
	    preferLowerReadUtil(PreferLowerReadUtil::False), inflightPenalty(1.0),
	    findTeamByServers(FindTeamByServers::True), src(std::move(servers)) {}

	// return true if a.score < b.score
	[[nodiscard]] bool lessCompare(TeamRef a, TeamRef b, int64_t aLoadBytes, int64_t bLoadBytes) const {
		int res = 0;
		if (forReadBalance) {
			res = preferLowerReadUtil ? greaterReadLoad(a, b) : lessReadLoad(a, b);
		}
		return res == 0 ? lessCompareByLoad(aLoadBytes, bLoadBytes) : res < 0;
	}

	std::string getDesc() const {
		std::stringstream ss;

		ss << "TeamSelect:" << teamSelect.toString() << " PreferLowerDiskUtil:" << preferLowerDiskUtil
		   << " teamMustHaveShards:" << teamMustHaveShards << " forReadBalance:" << forReadBalance
		   << " inflightPenalty:" << inflightPenalty << " findTeamByServers:" << findTeamByServers << ";";
		ss << "CompleteSources:";
		for (const auto& cs : completeSources) {
			ss << cs.toString() << ",";
		}

		return std::move(ss).str();
	}

private:
	// return true if preferHigherUtil && aLoadBytes <= bLoadBytes (higher load bytes has larger score)
	// or preferLowerUtil && aLoadBytes > bLoadBytes
	bool lessCompareByLoad(int64_t aLoadBytes, int64_t bLoadBytes) const {
		bool lessLoad = aLoadBytes <= bLoadBytes;
		return preferLowerDiskUtil ? !lessLoad : lessLoad;
	}

	// return -1 if a.readload > b.readload
	static int greaterReadLoad(TeamRef a, TeamRef b) {
		auto r1 = a->getLoadReadBandwidth(true), r2 = b->getLoadReadBandwidth(true);
		return r1 == r2 ? 0 : (r1 > r2 ? -1 : 1);
	}
	// return -1 if a.readload < b.readload
	static int lessReadLoad(TeamRef a, TeamRef b) {
		auto r1 = a->getLoadReadBandwidth(false), r2 = b->getLoadReadBandwidth(false);
		return r1 == r2 ? 0 : (r1 < r2 ? -1 : 1);
	}
};
