/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012-2018 Icinga Development Team (https://www.icinga.com/)  *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "redis/redisconnection.hpp"
#include "base/array.hpp"
#include "base/convert.hpp"
#include "base/defer.hpp"
#include "base/io-engine.hpp"
#include "base/logger.hpp"
#include "base/objectlock.hpp"
#include "base/string.hpp"
#include "base/tcpsocket.hpp"
#include <boost/asio/post.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/coroutine/exceptions.hpp>
#include <boost/utility/string_view.hpp>
#include <boost/variant/get.hpp>
#include <exception>
#include <iterator>
#include <memory>
#include <utility>

using namespace icinga;
namespace asio = boost::asio;

RedisConnection::RedisConnection(const String host, const int port, const String path, const String password, const int db) :
	RedisConnection(IoEngine::Get().GetIoService(), host, port, path, password, db)
{
}

RedisConnection::RedisConnection(boost::asio::io_context& io, String host, int port, String path, String password, int db)
	: m_Host(std::move(host)), m_Port(port), m_Path(std::move(path)), m_Password(std::move(password)), m_DbIndex(db),
	  m_Connecting(false), m_Connected(false), m_Started(false), m_Strand(io), m_QueuedWrites(io), m_QueuedReads(io)
{
}

void RedisConnection::Start()
{
	if (!m_Started.exchange(true)) {
		Ptr keepAlive (this);

		asio::spawn(m_Strand, [this, keepAlive](asio::yield_context yc) { ReadLoop(yc); });
		asio::spawn(m_Strand, [this, keepAlive](asio::yield_context yc) { WriteLoop(yc); });
	}

	if (!m_Connecting.exchange(true)) {
		Ptr keepAlive (this);

		asio::spawn(m_Strand, [this, keepAlive](asio::yield_context yc) { Connect(yc); });
	}
}

bool RedisConnection::IsConnected() {
	return m_Connected.load();
}

static inline
void LogQuery(RedisConnection::Query& query, Log& msg)
{
	int i = 0;

	for (auto& arg : query) {
		if (++i == 8) {
			msg << " ...";
			break;
		}

		msg << " '" << arg << '\'';
	}
}

void RedisConnection::FireAndForgetQuery(RedisConnection::Query query)
{
	{
		Log msg (LogNotice, "RedisWriter", "Firing and forgetting query:");
		LogQuery(query, msg);
	}

	auto item (std::make_shared<decltype(WriteQueueItem().FireAndForgetQuery)::element_type>(std::move(query)));

	asio::post(m_Strand, [this, item]() {
		m_Queues.Writes.emplace(WriteQueueItem{item, nullptr, nullptr, nullptr});
		m_QueuedWrites.Set();
	});
}

void RedisConnection::FireAndForgetQueries(RedisConnection::Queries queries)
{
	for (auto& query : queries) {
		Log msg (LogNotice, "RedisWriter", "Firing and forgetting query:");
		LogQuery(query, msg);
	}

	auto item (std::make_shared<decltype(WriteQueueItem().FireAndForgetQueries)::element_type>(std::move(queries)));

	asio::post(m_Strand, [this, item]() {
		m_Queues.Writes.emplace(WriteQueueItem{nullptr, item, nullptr, nullptr});
		m_QueuedWrites.Set();
	});
}

RedisConnection::Reply RedisConnection::GetResultOfQuery(RedisConnection::Query query)
{
	{
		Log msg (LogNotice, "RedisWriter", "Executing query:");
		LogQuery(query, msg);
	}

	std::promise<Reply> promise;
	auto future (promise.get_future());
	auto item (std::make_shared<decltype(WriteQueueItem().GetResultOfQuery)::element_type>(std::move(query), std::move(promise)));

	asio::post(m_Strand, [this, item]() {
		m_Queues.Writes.emplace(WriteQueueItem{nullptr, nullptr, item, nullptr});
		m_QueuedWrites.Set();
	});

	item = nullptr;
	future.wait();
	return future.get();
}

RedisConnection::Replies RedisConnection::GetResultsOfQueries(RedisConnection::Queries queries)
{
	for (auto& query : queries) {
		Log msg (LogNotice, "RedisWriter", "Executing query:");
		LogQuery(query, msg);
	}

	std::promise<Replies> promise;
	auto future (promise.get_future());
	auto item (std::make_shared<decltype(WriteQueueItem().GetResultsOfQueries)::element_type>(std::move(queries), std::move(promise)));

	asio::post(m_Strand, [this, item]() {
		m_Queues.Writes.emplace(WriteQueueItem{nullptr, nullptr, nullptr, item});
		m_QueuedWrites.Set();
	});

	item = nullptr;
	future.wait();
	return future.get();
}

void RedisConnection::Connect(asio::yield_context& yc)
{
	Defer notConnecting ([this]() { m_Connecting.store(m_Connected.load()); });

	Log(LogInformation, "RedisWriter", "Trying to connect to Redis server (async)");

	try {
		if (m_Path.IsEmpty()) {
			decltype(m_TcpConn) conn (new TcpConn(m_Strand.context()));
			icinga::Connect(conn->next_layer(), m_Host, Convert::ToString(m_Port), yc);
			m_TcpConn = std::move(conn);
		} else {
			decltype(m_UnixConn) conn (new UnixConn(m_Strand.context()));
			conn->next_layer().async_connect(Unix::endpoint(m_Path.CStr()), yc);
			m_UnixConn = std::move(conn);
		}

		m_Connected.store(true);

		Log(LogInformation, "RedisWriter", "Connected to Redis server");
	} catch (const boost::coroutines::detail::forced_unwind&) {
		throw;
	} catch (const std::exception& ex) {
		Log(LogCritical, "RedisWriter")
			<< "Cannot connect to " << m_Host << ":" << m_Port << ": " << ex.what();
	}
}

void RedisConnection::ReadLoop(asio::yield_context& yc)
{
	for (;;) {
		m_QueuedReads.Wait(yc);

		while (!m_Queues.FutureResponseActions.empty()) {
			auto item (std::move(m_Queues.FutureResponseActions.front()));
			m_Queues.FutureResponseActions.pop();

			switch (item.Action) {
				case ResponseAction::Ignore:
					try {
						for (auto i (item.Amount); i; --i) {
							ReadOne(yc);
						}
					} catch (const boost::coroutines::detail::forced_unwind&) {
						throw;
					} catch (const std::exception& ex) {
						Log(LogCritical, "RedisWriter")
							<< "Error during receiving the response to a query which has been fired and forgotten: " << ex.what();
						continue;
					} catch (...) {
						Log(LogCritical, "RedisWriter")
							<< "Error during receiving the response to a query which has been fired and forgotten";
						continue;
					}
					break;
				case ResponseAction::Deliver:
					for (auto i (item.Amount); i; --i) {
						auto promise (std::move(m_Queues.ReplyPromises.front()));
						m_Queues.ReplyPromises.pop();

						Reply reply;

						try {
							reply = ReadOne(yc);
						} catch (const boost::coroutines::detail::forced_unwind&) {
							throw;
						} catch (...) {
							promise.set_exception(std::current_exception());
							continue;
						}

						promise.set_value(std::move(reply));
					}
					break;
				case ResponseAction::DeliverBulk:
					{
						auto promise (std::move(m_Queues.RepliesPromises.front()));
						m_Queues.RepliesPromises.pop();

						Replies replies;
						replies.reserve(item.Amount);

						for (auto i (item.Amount); i; --i) {
							try {
								replies.emplace_back(ReadOne(yc));
							} catch (const boost::coroutines::detail::forced_unwind&) {
								throw;
							} catch (...) {
								promise.set_exception(std::current_exception());
								continue;
							}
						}

						promise.set_value(std::move(replies));
					}
			}
		}

		m_QueuedReads.Clear();
	}
}

void RedisConnection::WriteLoop(asio::yield_context& yc)
{
	for (;;) {
		m_QueuedWrites.Wait(yc);

		while (!m_Queues.Writes.empty()) {
			auto next (std::move(m_Queues.Writes.front()));
			m_Queues.Writes.pop();

			if (next.FireAndForgetQuery) {
				auto& item (*next.FireAndForgetQuery);

				try {
					WriteOne(item, yc);
				} catch (const boost::coroutines::detail::forced_unwind&) {
					throw;
				} catch (const std::exception& ex) {
					Log msg (LogCritical, "RedisWriter", "Error during sending query");
					LogQuery(item, msg);
					msg << " which has been fired and forgotten: " << ex.what();
					continue;
				} catch (...) {
					Log msg (LogCritical, "RedisWriter", "Error during sending query");
					LogQuery(item, msg);
					msg << " which has been fired and forgotten";
					continue;
				}

				if (m_Queues.FutureResponseActions.empty() || m_Queues.FutureResponseActions.back().Action != ResponseAction::Ignore) {
					m_Queues.FutureResponseActions.emplace(FutureResponseAction{1, ResponseAction::Ignore});
				} else {
					++m_Queues.FutureResponseActions.back().Amount;
				}

				m_QueuedReads.Set();
			}

			if (next.FireAndForgetQueries) {
				auto& item (*next.FireAndForgetQueries);
				size_t i = 0;

				try {
					for (auto& query : item) {
						WriteOne(query, yc);
						++i;
					}
				} catch (const boost::coroutines::detail::forced_unwind&) {
					throw;
				} catch (const std::exception& ex) {
					Log msg (LogCritical, "RedisWriter", "Error during sending query");
					LogQuery(item[i], msg);
					msg << " which has been fired and forgotten: " << ex.what();
					continue;
				} catch (...) {
					Log msg (LogCritical, "RedisWriter", "Error during sending query");
					LogQuery(item[i], msg);
					msg << " which has been fired and forgotten";
					continue;
				}

				if (m_Queues.FutureResponseActions.empty() || m_Queues.FutureResponseActions.back().Action != ResponseAction::Ignore) {
					m_Queues.FutureResponseActions.emplace(FutureResponseAction{item.size(), ResponseAction::Ignore});
				} else {
					m_Queues.FutureResponseActions.back().Amount += item.size();
				}

				m_QueuedReads.Set();
			}

			if (next.GetResultOfQuery) {
				auto& item (*next.GetResultOfQuery);

				try {
					WriteOne(item.first, yc);
				} catch (const boost::coroutines::detail::forced_unwind&) {
					throw;
				} catch (...) {
					item.second.set_exception(std::current_exception());
					continue;
				}

				m_Queues.ReplyPromises.emplace(std::move(item.second));

				if (m_Queues.FutureResponseActions.empty() || m_Queues.FutureResponseActions.back().Action != ResponseAction::Deliver) {
					m_Queues.FutureResponseActions.emplace(FutureResponseAction{1, ResponseAction::Deliver});
				} else {
					++m_Queues.FutureResponseActions.back().Amount;
				}

				m_QueuedReads.Set();
			}

			if (next.GetResultsOfQueries) {
				auto& item (*next.GetResultsOfQueries);

				try {
					for (auto& query : item.first) {
						WriteOne(query, yc);
					}
				} catch (const boost::coroutines::detail::forced_unwind&) {
					throw;
				} catch (...) {
					item.second.set_exception(std::current_exception());
					continue;
				}

				m_Queues.RepliesPromises.emplace(std::move(item.second));
				m_Queues.FutureResponseActions.emplace(FutureResponseAction{item.first.size(), ResponseAction::DeliverBulk});

				m_QueuedReads.Set();
			}
		}

		m_QueuedWrites.Clear();
	}
}

RedisConnection::Reply RedisConnection::ReadOne(boost::asio::yield_context& yc)
{
	if (m_Path.IsEmpty()) {
		return ReadOne(m_TcpConn, yc);
	} else {
		return ReadOne(m_UnixConn, yc);
	}
}

void RedisConnection::WriteOne(RedisConnection::Query& query, asio::yield_context& yc)
{
	if (m_Path.IsEmpty()) {
		WriteOne(m_TcpConn, query, yc);
	} else {
		WriteOne(m_UnixConn, query, yc);
	}
}
