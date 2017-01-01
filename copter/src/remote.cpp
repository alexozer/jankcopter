#include <SPI.h>
#include "logger.h"
#include "shm.h"
#include "config.h"
#include "remote.h"

Remote::Remote():
	m_firstRun{true},
	m_bluetooth{BLUEFRUIT_REQ_PIN, BLUEFRUIT_RDY_PIN, BLUEFRUIT_RST_PIN},
	m_lastState{ACI_EVT_DISCONNECTED} {}

void Remote::operator()() {
	// Must read bluetooth soon after begin()
	if (m_firstRun) {
		m_bluetooth.begin();
		m_firstRun = false;
	}

	readBluetooth();
	readStream(&Serial);
}

void Remote::readBluetooth() {
	m_bluetooth.pollACI();

	auto state = m_bluetooth.getState();
	if (state != m_lastState) {
		switch (state) {
			case ACI_EVT_DEVICE_STARTED:
				Logger::info("Bluetooth advertising started");
				break;
			case ACI_EVT_CONNECTED:
				Logger::info("Bluetooth connected!");
				break;
			case ACI_EVT_DISCONNECTED:
				Logger::info("Bluetooth disconnected or advertising timed out");
				break;
			default:
				// We don't care
				break;
		}
		m_lastState = state;
	}

	if (state == ACI_EVT_CONNECTED) {
		readStream(&m_bluetooth);
	}
}

void Remote::readStream(Stream* stream) {
	if (!stream->available()) return;

	// TODO rely on message framing instead of timing?
	size_t bytesRead = 0;
	while (bytesRead < sizeof(m_messageBuffer) && stream->available()) {
		m_messageBuffer[bytesRead++] = stream->read();
	}
	if (stream->available()) {
		Logger::error("Remote message larger than {} bytes, discarding", sizeof(m_messageBuffer));
		while (stream->available()) stream->read();
		return;
	}

	ShmMsg update = ShmMsg_init_zero;
	auto pbUpdateStream = pb_istream_from_buffer(m_messageBuffer, bytesRead);
	update.var = {&decodeVar, nullptr};
	if (!pb_decode_noinit(&pbUpdateStream, ShmMsg_fields, &update)) {
		Logger::error("Failed to decode remote message: {}",
				PB_GET_ERROR(&pbUpdateStream));
	}
}

bool Remote::decodeVar(pb_istream_t* stream, const pb_field_t* field, void** arg) {
	ShmMsg_Var var = ShmMsg_Var_init_zero;
	if (!pb_decode_noinit(stream, ShmMsg_Var_fields, &var)) {
		Logger::error("Failed to decode remote variable: {}",
				PB_GET_ERROR(stream));
		return false;
	}

	Shm::Var::Type msgType;
	if (var.has_intValue) msgType = Shm::Var::Type::INT;
	else if (var.has_floatValue) msgType = Shm::Var::Type::FLOAT;
	else if (var.has_boolValue) msgType = Shm::Var::Type::BOOL;
	else msgType = Shm::Var::Type::STRING;

	auto shmVar = Shm::var(var.tag);
	auto shmVarType = shmVar->type();
	if (msgType != shmVarType) {
		Logger::error("Remote var type mismatch: expected {}, got {}",
				Shm::typeString(shmVarType),
				Shm::typeString(msgType));
		return true;
	}

	switch (msgType) {
		case Shm::Var::Type::INT:
			shmVar->set((int)var.intValue);
			break;
		case Shm::Var::Type::FLOAT:
			shmVar->set(var.floatValue);
			break;
		case Shm::Var::Type::BOOL:
			shmVar->set(var.boolValue);
			break;
		default:
			Logger::error("Unsupported remote var type");
			break;
	}

	return true;
}
