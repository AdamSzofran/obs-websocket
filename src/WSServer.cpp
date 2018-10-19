/*
obs-websocket
Copyright (C) 2016-2017	Stéphane Lepin <stephane.lepin@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <QtCore/QThread>
#include <QtCore/QByteArray>
#include <QMainWindow>
#include <QMessageBox>
#include <QtConcurrent/QtConcurrent>
#include <obs-frontend-api.h>

#include "WSServer.h"
#include "obs-websocket.h"
#include "Config.h"
#include "Utils.h"

QT_USE_NAMESPACE

using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

WSServer* WSServer::Instance = nullptr;

QString decodeBase64(const QString& source)
{
	return QString::fromUtf8(
		QByteArray::fromBase64(
			source.toUtf8()
		)
	);
}

WSServer::WSServer(QObject* parent)
	: QObject(parent),
	  _connections(),
	  _clMutex(QMutex::Recursive)
{
	_server.init_asio();

	_server.set_validate_handler(bind(&WSServer::validateConnection, this, ::_1));
	_server.set_open_handler(bind(&WSServer::onOpen, this, ::_1));
	_server.set_close_handler(bind(&WSServer::onClose, this, ::_1));
	_server.set_message_handler(bind(&WSServer::onMessage, this, ::_1, ::_2));
}

WSServer::~WSServer()
{
	stop();
}

void WSServer::start(quint16 port)
{
	if (_server.is_listening() && port == _serverPort) {
		blog(LOG_INFO, "WebSocketsServer::start: server already on this port. no restart needed");
		return;
	}

	if (_server.is_listening()) {
		stop();
	}

	_serverPort = port;

	_server.listen(_serverPort);
	_server.start_accept();

	QtConcurrent::run([=]() {
		_server.run();
	});

	blog(LOG_INFO, "server started successfully on port %d", _serverPort);
}

void WSServer::stop()
{
	_server.stop();
	_server.stop_listening();
	blog(LOG_INFO, "server stopped successfully");
}

void WSServer::broadcast(QString message)
{
	QMutexLocker locker(&_clMutex);
	for (connection_hdl hdl : _connections) {
		_server.send(hdl, message.toStdString(), websocketpp::frame::opcode::text);
	}
}

bool WSServer::validateConnection(connection_hdl hdl)
{
	// TODO enforce subprotocol

	Config* config = Config::Current();
	if (config->AuthRequired) {
		auto conn = _server.get_con_from_hdl(hdl);

		QString authorization =
				QString::fromStdString(conn->get_request_header("Authorization"));
		if (!authorization.isNull() && !authorization.isEmpty()) {
			const QStringList& parts = authorization.split(" ", QString::SplitBehavior::SkipEmptyParts);
			if (parts.length() >= 2) {
				const QString& authType = parts.at(0);
				const QString& authValue = parts.at(1);

				if (authType == "Basic") {
					const QStringList& decodedParts =
							decodeBase64(authValue).split(":", QString::SplitBehavior::SkipEmptyParts);
					if (decodedParts.length() >= 2) {
						const QString& username = decodedParts.at(0);
						const QString& password = decodedParts.at(1);

						// TODO time-constant string comparison
						if (password == config->AuthPassword) {
							return true;
						}
					}
				}
			}
		}

		conn->set_status(websocketpp::http::status_code::unauthorized);
		conn->append_header("WWW-Authenticate", "Basic charset=\"UTF-8\"");
		return false;
	}

	return true;
}

void WSServer::onOpen(connection_hdl hdl)
{
	QMutexLocker locker(&_clMutex);
	_connections.insert(hdl);
	locker.unlock();

	QString clientIp = getRemoteEndpoint(hdl);
	notifyConnection(clientIp);
	blog(LOG_INFO, "new client connection from %s", clientIp.toUtf8().constData());
}

void WSServer::onMessage(connection_hdl hdl, server::message_ptr message)
{
	auto opcode = message->get_opcode();
	if (opcode != websocketpp::frame::opcode::text) {
		return;
	}

	QString payload = QString::fromStdString(message->get_payload());

	// TODO refactor handler
	WSRequestHandler handler;
	handler.processIncomingMessage(payload);
	std::string response = handler.getResponse().toStdString();

	_server.send(hdl, response, websocketpp::frame::opcode::text);
}

void WSServer::onClose(connection_hdl hdl)
{
	QMutexLocker locker(&_clMutex);
	_connections.erase(hdl);
	locker.unlock();

	QString clientIp = getRemoteEndpoint(hdl);
	notifyDisconnection(clientIp);
	blog(LOG_INFO, "client %s disconnected", clientIp.toUtf8().constData());
}

QString WSServer::getRemoteEndpoint(connection_hdl hdl)
{
	auto conn = _server.get_con_from_hdl(hdl);
	return QString::fromStdString(conn->get_remote_endpoint());
}

void WSServer::notifyConnection(QString clientIp)
{
	obs_frontend_push_ui_translation(obs_module_get_string);
	QString title = tr("OBSWebsocket.NotifyConnect.Title");
	QString msg = tr("OBSWebsocket.NotifyConnect.Message").arg(clientIp);
	obs_frontend_pop_ui_translation();

	Utils::SysTrayNotify(msg, QSystemTrayIcon::Information, title);
}

void WSServer::notifyDisconnection(QString clientIp)
{
	obs_frontend_push_ui_translation(obs_module_get_string);
	QString title = tr("OBSWebsocket.NotifyDisconnect.Title");
	QString msg = tr("OBSWebsocket.NotifyDisconnect.Message").arg(clientIp);
	obs_frontend_pop_ui_translation();

	Utils::SysTrayNotify(msg, QSystemTrayIcon::Information, title);
}
