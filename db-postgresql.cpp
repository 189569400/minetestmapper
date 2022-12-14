#include <stdexcept>
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <arpa/inet.h>
#include "db-postgresql.h"
#include "util.h"
#include "types.h"

#define ARRLEN(x) (sizeof(x) / sizeof((x)[0]))

DBPostgreSQL::DBPostgreSQL(const std::string &mapdir)
{
	std::ifstream ifs(mapdir + "world.mt");
	if (!ifs.good())
		throw std::runtime_error("Failed to read world.mt");
	std::string connect_string = read_setting("pgsql_connection", ifs);
	ifs.close();
	db = PQconnectdb(connect_string.c_str());

	if (PQstatus(db) != CONNECTION_OK) {
		throw std::runtime_error(std::string(
			"PostgreSQL database error: ") +
			PQerrorMessage(db)
		);
	}

	prepareStatement(
		"get_block_pos",
		"SELECT posX::int4, posY::int4, posZ::int4 FROM blocks WHERE"
		" (posX BETWEEN $1::int4 AND $2::int4) AND"
		" (posY BETWEEN $3::int4 AND $4::int4) AND"
		" (posZ BETWEEN $5::int4 AND $6::int4)"
	);
	prepareStatement(
		"get_blocks",
		"SELECT posY::int4, data FROM blocks WHERE"
		" posX = $1::int4 AND posZ = $2::int4"
		" AND (posY BETWEEN $3::int4 AND $4::int4)"
	);
	prepareStatement(
		"get_block_exact",
		"SELECT data FROM blocks WHERE"
		" posX = $1::int4 AND posY = $2::int4 AND posZ = $3::int4"
	);

	checkResults(PQexec(db, "START TRANSACTION;"));
	checkResults(PQexec(db, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;"));
}


DBPostgreSQL::~DBPostgreSQL()
{
	try {
		checkResults(PQexec(db, "COMMIT;"));
	} catch (const std::exception& caught) {
		std::cerr << "could not finalize: " << caught.what() << std::endl;
	}
	PQfinish(db);
}


std::vector<BlockPos> DBPostgreSQL::getBlockPos(BlockPos min, BlockPos max)
{
	int32_t const x1 = htonl(min.x);
	int32_t const x2 = htonl(max.x - 1);
	int32_t const y1 = htonl(min.y);
	int32_t const y2 = htonl(max.y - 1);
	int32_t const z1 = htonl(min.z);
	int32_t const z2 = htonl(max.z - 1);

	const void *args[] = { &x1, &x2, &y1, &y2, &z1, &z2 };
	const int argLen[] = { 4, 4, 4, 4, 4, 4 };
	const int argFmt[] = { 1, 1, 1, 1, 1, 1 };

	PGresult *results = execPrepared(
		"get_block_pos", ARRLEN(args), args,
		argLen, argFmt, false
	);

	int numrows = PQntuples(results);

	std::vector<BlockPos> positions;
	positions.reserve(numrows);

	for (int row = 0; row < numrows; ++row)
		positions.emplace_back(pg_to_blockpos(results, row, 0));

	PQclear(results);

	return positions;
}


void DBPostgreSQL::getBlocksOnXZ(BlockList &blocks, int16_t xPos, int16_t zPos,
		int16_t min_y, int16_t max_y)
{
	int32_t const x = htonl(xPos);
	int32_t const z = htonl(zPos);
	int32_t const y1 = htonl(min_y);
	int32_t const y2 = htonl(max_y - 1);

	const void *args[] = { &x, &z, &y1, &y2 };
	const int argLen[] = { 4, 4, 4, 4 };
	const int argFmt[] = { 1, 1, 1, 1 };

	PGresult *results = execPrepared(
		"get_blocks", ARRLEN(args), args,
		argLen, argFmt, false
	);

	int numrows = PQntuples(results);

	for (int row = 0; row < numrows; ++row) {
		BlockPos position;
		position.x = xPos;
		position.y = pg_binary_to_int(results, row, 0);
		position.z = zPos;
		blocks.emplace_back(
			position,
			ustring(
				reinterpret_cast<unsigned char*>(
					PQgetvalue(results, row, 1)
				),
				PQgetlength(results, row, 1)
			)
		);
	}

	PQclear(results);
}


void DBPostgreSQL::getBlocksByPos(BlockList &blocks,
			const std::vector<BlockPos> &positions)
{
	int32_t x, y, z;

	const void *args[] = { &x, &y, &z };
	const int argLen[] = { 4, 4, 4 };
	const int argFmt[] = { 1, 1, 1 };

	for (auto pos : positions) {
		x = htonl(pos.x);
		y = htonl(pos.y);
		z = htonl(pos.z);

		PGresult *results = execPrepared(
			"get_block_exact", ARRLEN(args), args,
			argLen, argFmt, false
		);

		if (PQntuples(results) > 0) {
			blocks.emplace_back(
				pos,
				ustring(
					reinterpret_cast<unsigned char*>(
						PQgetvalue(results, 0, 0)
					),
					PQgetlength(results, 0, 0)
				)
			);
		}

		PQclear(results);
	}
}


PGresult *DBPostgreSQL::checkResults(PGresult *res, bool clear)
{
	ExecStatusType statusType = PQresultStatus(res);

	switch (statusType) {
	case PGRES_COMMAND_OK:
	case PGRES_TUPLES_OK:
		break;
	case PGRES_FATAL_ERROR:
		throw std::runtime_error(
			std::string("PostgreSQL database error: ") +
			PQresultErrorMessage(res)
		);
	default:
		throw std::runtime_error(
			"Unhandled PostgreSQL result code"
		);
	}

	if (clear)
		PQclear(res);

	return res;
}

void DBPostgreSQL::prepareStatement(const std::string &name, const std::string &sql)
{
	checkResults(PQprepare(db, name.c_str(), sql.c_str(), 0, NULL));
}

PGresult *DBPostgreSQL::execPrepared(
	const char *stmtName, const int paramsNumber,
	const void **params,
	const int *paramsLengths, const int *paramsFormats,
	bool clear
)
{
	return checkResults(PQexecPrepared(db, stmtName, paramsNumber,
		(const char* const*) params, paramsLengths, paramsFormats,
		1 /* binary output */), clear
	);
}

int DBPostgreSQL::pg_binary_to_int(PGresult *res, int row, int col)
{
	int32_t* raw = reinterpret_cast<int32_t*>(PQgetvalue(res, row, col));
	return ntohl(*raw);
}

BlockPos DBPostgreSQL::pg_to_blockpos(PGresult *res, int row, int col)
{
	BlockPos result;
	result.x = pg_binary_to_int(res, row, col);
	result.y = pg_binary_to_int(res, row, col + 1);
	result.z = pg_binary_to_int(res, row, col + 2);
	return result;
}
