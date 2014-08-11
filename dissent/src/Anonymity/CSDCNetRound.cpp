/**
 * Handle false accusations
 * Implement misbehaving servers
 * Implement colluding server
 * Eventually handle "light weight" consensus amongst all non-colluding servers when a server equivocates
 * Consider how to have server exchange ciphertext bits ... already know both colluding parties one needs to submit the shared secret
 */

#include "Crypto/DsaPrivateKey.hpp"
#include "Crypto/DsaPublicKey.hpp"
#include "Crypto/Hash.hpp"
#include "Identity/PublicIdentity.hpp"
#include "Utils/Random.hpp"
#include "Utils/QRunTimeError.hpp"
#include "Utils/Serialization.hpp"
#include "Utils/Time.hpp"
#include "Utils/Timer.hpp"
#include "Utils/TimerCallback.hpp"
#include "Utils/Utils.hpp"

#include "NeffKeyShuffleRound.hpp"
#include "NeffShuffleRound.hpp"
#include "NullRound.hpp"
#include "CSDCNetRound.hpp"

#ifdef CS_BLOG_DROP
#include "BlogDropRound.hpp"
#endif

namespace Dissent {
  using Crypto::CryptoRandom;
  using Crypto::Hash;
  using Identity::PublicIdentity;
  using Utils::QRunTimeError;
  using Utils::Serialization;

namespace Anonymity {
  CSDCNetRound::CSDCNetRound(const Identity::Roster &clients,
      const Identity::Roster &servers,
      const Identity::PrivateIdentity &ident,
      const QByteArray &nonce,
      const QSharedPointer<ClientServer::Overlay> &overlay,
      Messaging::GetDataCallback &get_data,
      CreateRound create_shuffle) :
    BaseDCNetRound(clients, servers, ident, nonce, overlay, get_data, create_shuffle),
    _state_machine(this),
    _stop_next(false),
    _get_blame_data(this, &CSDCNetRound::GetBlameData)
  {
    _state_machine.AddState(OFFLINE);
    _state_machine.AddState(SHUFFLING, -1, 0, &CSDCNetRound::StartShuffle);
    _state_machine.AddState(PREPARE_FOR_BULK, -1, 0,
        &CSDCNetRound::PrepareForBulk);
    _state_machine.AddState(STARTING_BLAME_SHUFFLE, -1, 0,
        &CSDCNetRound::StartBlameShuffle);
    _state_machine.AddState(WAITING_FOR_BLAME_SHUFFLE, -1, 0,
        &CSDCNetRound::ProcessBlameShuffle);
    _state_machine.AddState(FINISHED);

#ifdef CS_BLOG_DROP
    _state_machine.AddState(PROCESS_BOOTSTRAP, -1, 0,
        &CSDCNetRound::ProcessBlogDrop);
#else
    if(GetShuffleRound().dynamicCast<NeffKeyShuffleRound>()) {
      _state_machine.AddState(PROCESS_BOOTSTRAP, -1, 0,
          &CSDCNetRound::ProcessKeyShuffle);
    } else {
      _state_machine.AddState(PROCESS_BOOTSTRAP, -1, 0,
          &CSDCNetRound::ProcessDataShuffle);
    }
#endif

    _state_machine.AddTransition(OFFLINE, SHUFFLING);
    _state_machine.AddTransition(SHUFFLING, PROCESS_BOOTSTRAP);
    _state_machine.AddTransition(PROCESS_BOOTSTRAP, PREPARE_FOR_BULK);
    _state_machine.AddTransition(STARTING_BLAME_SHUFFLE,
        WAITING_FOR_BLAME_SHUFFLE);
    _state_machine.SetState(OFFLINE);

    if(IsServer()) {
      InitServer();
    } else {
      InitClient();
    }

    _state->slot_open = false;

    Hash hashalgo;
    QByteArray hashval = hashalgo.ComputeHash(GetNonce());
    hashval = hashalgo.ComputeHash(hashval);

#ifdef CS_BLOG_DROP
    /// XXX Need to figure out header code for this stuff
    QSharedPointer<BlogDropRound> bdr(new BlogDropRound(
          Crypto::BlogDrop::Parameters::CppECHashingProduction(),
          GetClients(), GetServers(), GetPrivateIdentity(), hashval,
          GetOverlay(), TCreateRound<NeffShuffleRound>));
    bdr->SetSharedPointer(bdr);
    bdr->SetHeaderBytes(header);
    bdr->SetInteractiveMode();

    SetShuffleRound(bdr);
    _state->blame_shuffle = bdr;

    QObject::connect(bdr.data(), SIGNAL(ReadyForInteraction()),
        this, SLOT(OperationFinished()));
#else
    QSharedPointer<NeffKeyShuffleRound> nks =
      GetShuffleRound().dynamicCast<NeffKeyShuffleRound>();
    if(!nks) {
      _state->blame_shuffle = QSharedPointer<Round>(new NullRound(GetClients(),
            GetServers(), GetPrivateIdentity(), hashval, GetOverlay(),
            _get_blame_data));
    } else {
      _state->blame_shuffle = QSharedPointer<Round>(new NeffShuffleRound(GetClients(),
            GetServers(), GetPrivateIdentity(), hashval, GetOverlay(),
            _get_blame_data));
    }

    QObject::connect(_state->blame_shuffle.data(), SIGNAL(Finished()),
        this, SLOT(OperationFinished()));
    QByteArray header = GetHeaderBytes();
    header[1] = 2;
    _state->blame_shuffle->SetHeaderBytes(header);
#endif
    _state->blame_shuffle->SetSink(&_blame_sink);
  }

  void CSDCNetRound::InitServer()
  {
    _server_state = QSharedPointer<ServerState>(new ServerState());
    _state = _server_state;
    Q_ASSERT(_state);
    _server_state->handled_servers_bits = QBitArray(GetClients().Count(), false);

    _server_state->current_phase_log =
      QSharedPointer<PhaseLog>(
          new PhaseLog(_state_machine.GetPhase(), GetClients().Count()));
    _server_state->phase_logs[_state_machine.GetPhase()] =
      _server_state->current_phase_log;

#ifndef CSBR_RECONNECTS
    foreach(const QSharedPointer<Connections::Connection> &con,
        GetOverlay()->GetConnectionTable().GetConnections())
    {
      if(GetOverlay()->IsServer(con->GetRemoteId())) {
        continue;
      }

      _server_state->allowed_clients.insert(con->GetRemoteId());
    }
#endif
    _server_state->handled_clients.fill(false, GetClients().Count());

    _state_machine.AddState(SERVER_WAIT_FOR_CLIENT_CIPHERTEXT,
        CLIENT_CIPHERTEXT, &CSDCNetRound::HandleClientCiphertext,
        &CSDCNetRound::SetOnlineClients);
    _state_machine.AddState(SERVER_WAIT_FOR_CLIENT_LISTS,
        SERVER_CLIENT_LIST, &CSDCNetRound::HandleServerClientList,
        &CSDCNetRound::SubmitClientList);
    _state_machine.AddState(SERVER_WAIT_FOR_SERVER_COMMITS,
        SERVER_COMMIT, &CSDCNetRound::HandleServerCommit,
        &CSDCNetRound::SubmitCommit);
    _state_machine.AddState(SERVER_WAIT_FOR_SERVER_CIPHERTEXT,
        SERVER_CIPHERTEXT, &CSDCNetRound::HandleServerCiphertext,
        &CSDCNetRound::SubmitServerCiphertext);
    _state_machine.AddState(SERVER_WAIT_FOR_SERVER_VALIDATION,
        SERVER_VALIDATION, &CSDCNetRound::HandleServerValidation,
        &CSDCNetRound::SubmitValidation);
    _state_machine.AddState(SERVER_PUSH_CLEARTEXT, -1, 0,
        &CSDCNetRound::PushCleartext);
    _state_machine.AddState(SERVER_TRANSMIT_BLAME_BITS, -1, 0,
        &CSDCNetRound::TransmitBlameBits);
    _state_machine.AddState(SERVER_WAITING_FOR_BLAME_BITS, SERVER_BLAME_BITS,
        &CSDCNetRound::HandleBlameBits);
    _state_machine.AddState(SERVER_REQUEST_CLIENT_REBUTTAL, -1, 0,
        &CSDCNetRound::RequestRebuttal);
    _state_machine.AddState(SERVER_WAIT_FOR_CLIENT_REBUTTAL, CLIENT_REBUTTAL,
        &CSDCNetRound::HandleRebuttal);
    _state_machine.AddState(SERVER_EXCHANGE_VERDICT_SIGNATURE, -1, 0,
        &CSDCNetRound::SubmitVerdictSignature);
    _state_machine.AddState(SERVER_SHARE_VERDICT, -1, 0,
        &CSDCNetRound::PushVerdict);
    _state_machine.AddState(SERVER_WAIT_FOR_VERDICT_SIGNATURE,
        SERVER_VERDICT_SIGNATURE, &CSDCNetRound::HandleVerdictSignature);

    _state_machine.AddTransition(PREPARE_FOR_BULK,
        SERVER_WAIT_FOR_CLIENT_CIPHERTEXT);
    _state_machine.AddTransition(SERVER_WAIT_FOR_CLIENT_CIPHERTEXT,
        SERVER_WAIT_FOR_CLIENT_LISTS);
    _state_machine.AddTransition(SERVER_WAIT_FOR_CLIENT_LISTS,
        SERVER_WAIT_FOR_SERVER_COMMITS);
    _state_machine.AddTransition(SERVER_WAIT_FOR_SERVER_COMMITS,
        SERVER_WAIT_FOR_SERVER_CIPHERTEXT);
    _state_machine.AddTransition(SERVER_WAIT_FOR_SERVER_CIPHERTEXT,
        SERVER_WAIT_FOR_SERVER_VALIDATION);
    _state_machine.AddTransition(SERVER_WAIT_FOR_SERVER_VALIDATION,
        SERVER_PUSH_CLEARTEXT);
    _state_machine.AddTransition(SERVER_PUSH_CLEARTEXT,
        SERVER_WAIT_FOR_CLIENT_CIPHERTEXT);

    _state_machine.SetCycleState(SERVER_PUSH_CLEARTEXT);

    _state_machine.AddTransition(WAITING_FOR_BLAME_SHUFFLE,
        SERVER_TRANSMIT_BLAME_BITS);
    _state_machine.AddTransition(SERVER_TRANSMIT_BLAME_BITS,
        SERVER_WAITING_FOR_BLAME_BITS);
    _state_machine.AddTransition(SERVER_WAITING_FOR_BLAME_BITS,
        SERVER_REQUEST_CLIENT_REBUTTAL);
    _state_machine.AddTransition(SERVER_REQUEST_CLIENT_REBUTTAL,
        SERVER_WAIT_FOR_CLIENT_REBUTTAL);
    _state_machine.AddTransition(SERVER_WAIT_FOR_CLIENT_REBUTTAL,
        SERVER_EXCHANGE_VERDICT_SIGNATURE);
    _state_machine.AddTransition(SERVER_EXCHANGE_VERDICT_SIGNATURE,
        SERVER_WAIT_FOR_VERDICT_SIGNATURE);
    _state_machine.AddTransition(SERVER_WAIT_FOR_VERDICT_SIGNATURE,
        SERVER_SHARE_VERDICT);
  }

  void CSDCNetRound::InitClient()
  {
    _state = QSharedPointer<State>(new State());
    foreach(const QSharedPointer<Connections::Connection> &con,
        GetOverlay()->GetConnectionTable().GetConnections())
    {
      if(GetOverlay()->IsServer(con->GetRemoteId())) {
        _state->my_server = con->GetRemoteId();
        break;
      }
    }

    _state_machine.AddState(CLIENT_WAIT_FOR_CLEARTEXT,
        SERVER_CLEARTEXT, &CSDCNetRound::HandleServerCleartext,
        &CSDCNetRound::SubmitClientCiphertext);
    _state_machine.AddState(WAITING_FOR_DATA_REQUEST_OR_VERDICT,
        SERVER_REBUTTAL_OR_VERDICT, &CSDCNetRound::HandleRebuttalOrVerdict);

    _state_machine.AddTransition(PREPARE_FOR_BULK,
        CLIENT_WAIT_FOR_CLEARTEXT);
    _state_machine.AddTransition(CLIENT_WAIT_FOR_CLEARTEXT,
        CLIENT_WAIT_FOR_CLEARTEXT);

    _state_machine.SetCycleState(CLIENT_WAIT_FOR_CLEARTEXT);

    _state_machine.AddTransition(WAITING_FOR_BLAME_SHUFFLE,
        WAITING_FOR_DATA_REQUEST_OR_VERDICT);
  }

  CSDCNetRound::~CSDCNetRound()
  {
    if(IsServer()) {
      _server_state->client_ciphertext_period.Stop();
    }
  }

  void CSDCNetRound::OnStart()
  {
    Round::OnStart();
    _state_machine.StateComplete();
  }

  void CSDCNetRound::OnStop()
  {
    if(IsServer()) {
      _server_state->client_ciphertext_period.Stop();
    }

    _state_machine.SetState(FINISHED);
    Utils::PrintResourceUsage(ToString() + " " + "finished bulk");
    Round::OnStop();
  }

  void CSDCNetRound::HandleDisconnect(const Connections::Id &id)
  {
    if(!GetServers().Contains(id) && !GetClients().Contains(id)) {
      return;
    }

#ifndef CSBR_RECONNECTS
    if(IsServer() && GetClients().Contains(id)) {
      _server_state->allowed_clients.remove(id);
    }
#endif

    if((_state_machine.GetState() == OFFLINE) ||
        (_state_machine.GetState() == SHUFFLING))
    {
      GetShuffleRound()->HandleDisconnect(id);
    } else if(GetServers().Contains(id)) {
      qDebug() << "A server (" << id << ") disconnected.";
      SetInterrupted();
      Stop("A server (" + id.ToString() +") disconnected.");
    } else {
      qDebug() << "A client (" << id << ") disconnected, ignoring.";
    }
  }

  void CSDCNetRound::BeforeStateTransition()
  {
    if(_server_state) {
      _server_state->client_ciphertext_period.Stop();
      _server_state->handled_servers.clear();
    }
  }

  bool CSDCNetRound::CycleComplete()
  {
    if(_server_state) {
      _server_state->handled_clients.fill(false, GetClients().Count());
      _server_state->client_ciphertexts.clear();
      _server_state->server_ciphertexts.clear();

      int nphase = _state_machine.GetPhase() + 1;
      if(nphase >= 5) {
        Q_ASSERT(_server_state->phase_logs.remove(nphase - 5));
      }
      _server_state->current_phase_log =
        QSharedPointer<PhaseLog>(
            new PhaseLog(nphase, GetClients().Count()));
      _server_state->phase_logs[nphase] = _server_state->current_phase_log;
    }

    if(_stop_next) {
      SetInterrupted();
      Stop("Stopped for join");
      return false;
    }
    return true;
  }

  void CSDCNetRound::ProcessPacket(const Connections::Id &from,
      const QByteArray &data)
  {
    if(data.size() == 0) {
      qWarning() << "Invalid data";
      return;
    }

    qint8 type = data[0];
    switch(type) {
      case 0:
        _state_machine.ProcessData(from, data.mid(1));
        break;
      case 1:
        GetShuffleRound()->ProcessPacket(from, data.mid(1));
        break;
      case 2:
        _state->blame_shuffle->ProcessPacket(from, data.mid(1));
        break;
      default:
        qWarning() << "Unknown packet type:" << type;
    }
  }

  void CSDCNetRound::HandleServerCleartext(const Connections::Id &from, QDataStream &stream)
  {
    if(IsServer()) {
      throw QRunTimeError("Not a client");
    } else if(_state->my_server != from) {
      throw QRunTimeError("Not a server");
    }

    QHash<int, QByteArray> signatures;
    QByteArray cleartext;
    QBitArray online;
    stream >> signatures >> cleartext >> online;

    if(cleartext.size() != _state->msg_length) {
      throw QRunTimeError("Cleartext size mismatch: " +
          QString::number(cleartext.size()) + " :: " +
          QString::number(_state->msg_length));
    }

    Hash hash;
    hash.Update(cleartext);

    QByteArray data;
    QDataStream tstream(&data, QIODevice::WriteOnly);
    tstream << online;
    hash.Update(data);

    QByteArray signed_hash = hash.ComputeHash();

    int server_length = GetServers().Count();
    for(int idx = 0; idx < server_length; idx++) {
      if(!GetServers().GetKey(idx)->Verify(signed_hash, signatures[idx])) {
        Stop("Failed to verify signatures");
        return;
      }
    }

    _state->cleartext = cleartext;
    ProcessCleartext();

    if(_state->start_accuse) {
      _state_machine.SetState(STARTING_BLAME_SHUFFLE);
    } else {
      _state_machine.StateComplete();
    }
  }

  void CSDCNetRound::HandleClientCiphertext(const Connections::Id &from, QDataStream &stream)
  {
    if(!IsServer()) {
      throw QRunTimeError("Not a server");
    }

    Q_ASSERT(_server_state);
    int idx = GetClients().GetIndex(from);

    if(!_server_state->allowed_clients.contains(from)) {
      throw QRunTimeError("Not allowed to submit a ciphertext");
    } else if(_server_state->handled_clients.at(idx)) {
      throw QRunTimeError("Already have ciphertext");
    }

    QByteArray payload;
    stream >> payload;

    if(payload.size() != _server_state->msg_length) {
      throw QRunTimeError("Incorrect message length, got " +
          QString::number(payload.size()) + " expected " +
          QString::number(_server_state->msg_length));
    }

    _server_state->handled_clients[idx] = true;
    _server_state->client_ciphertexts.append(QPair<int, QByteArray>(idx, payload));
    _server_state->current_phase_log->messages[idx] = payload;

    qDebug() << GetServers().GetIndex(GetLocalId()) << GetLocalId().ToString() <<
      ": received client ciphertext from" << GetClients().GetIndex(from) <<
      from.ToString() << "Have" << _server_state->client_ciphertexts.count()
      << "expecting" << _server_state->allowed_clients.count();

    if(_server_state->allowed_clients.count() ==
        _server_state->client_ciphertexts.count())
    {
      _state_machine.StateComplete();
    } else if(_server_state->client_ciphertexts.count() ==
        _server_state->expected_clients)
    {
      // Start the flexible deadline
      _server_state->client_ciphertext_period.Stop();
      int window = Utils::Time::GetInstance().MSecsSinceEpoch() -
        _server_state->start_of_phase;
      Utils::TimerCallback *cb = new Utils::TimerMethod<CSDCNetRound, int>(
          this, &CSDCNetRound::ConcludeClientCiphertextSubmission, 0);
      _server_state->client_ciphertext_period =
        Utils::Timer::GetInstance().QueueCallback(cb, window);

      qDebug() << GetServers().GetIndex(GetLocalId()) << GetLocalId().ToString() <<
        "setting client submission flex-deadline:" << window;
    }
  }

  void CSDCNetRound::HandleServerClientList(const Connections::Id &from, QDataStream &stream)
  {
    if(!GetOverlay()->IsServer(from)) {
      throw QRunTimeError("Not a server");
    }

    Q_ASSERT(_server_state);

    if(_server_state->handled_servers.contains(from)) {
      throw QRunTimeError("Already have client list");
    }

    QBitArray clients;
    stream >> clients;

    /// XXX Handle overlaps in list

    _server_state->handled_clients |= clients;
    _server_state->handled_servers.insert(from);

    int sidx = GetServers().GetIndex(from);
    for(int idx = 0; idx < clients.size(); idx++) {
      if(clients.at(0)) {
        _server_state->current_phase_log->client_to_server[idx] = sidx;
      }
    }

    qDebug() << GetServers().GetIndex(GetLocalId()) << GetLocalId().ToString() <<
      ": received client list from" << GetServers().GetIndex(from) <<
      from.ToString() << "Have" << _server_state->handled_servers.count()
      << "expecting" << GetServers().Count();

    if(_server_state->handled_servers.count() == GetServers().Count()) {
      _state_machine.StateComplete();
    }
  }

  void CSDCNetRound::HandleServerCommit(const Connections::Id &from, QDataStream &stream)
  {
    if(!IsServer()) {
      throw QRunTimeError("Not a server");
    } else if(!GetServers().Contains(from)) {
      throw QRunTimeError("Not a server");
    }

    Q_ASSERT(_server_state);

    if(_server_state->handled_servers.contains(from)) {
      throw QRunTimeError("Already have commit");
    }

    QByteArray commit;
    stream >> commit;

    _server_state->handled_servers.insert(from);
    _server_state->server_commits[GetServers().GetIndex(from)] = commit;

    qDebug() << GetServers().GetIndex(GetLocalId()) << GetLocalId().ToString() <<
      ": received commit from" << GetServers().GetIndex(from) <<
      from.ToString() << "Have" << _server_state->handled_servers.count()
      << "expecting" << GetServers().Count();

    if(_server_state->handled_servers.count() == GetServers().Count()) {
      _state_machine.StateComplete();
    }
  }

  void CSDCNetRound::HandleServerCiphertext(const Connections::Id &from, QDataStream &stream)
  {
    if(!IsServer()) {
      throw QRunTimeError("Not a server");
    } else if(!GetServers().Contains(from)) {
      throw QRunTimeError("Not a server");
    }

    Q_ASSERT(_server_state);

    if(_server_state->handled_servers.contains(from)) {
      throw QRunTimeError("Already have ciphertext");
    }

    QByteArray ciphertext;
    stream >> ciphertext;

    if(ciphertext.size() != _server_state->msg_length) {
      throw QRunTimeError("Incorrect message length, got " +
          QString::number(ciphertext.size()) + " expected " +
          QString::number(_server_state->msg_length));
    }

    QByteArray commit = Hash().ComputeHash(ciphertext);

    if(commit != _server_state->server_commits[
        GetServers().GetIndex(from)])
    {
      throw QRunTimeError("Does not match commit.");
    }

    _server_state->handled_servers.insert(from);
    _server_state->server_ciphertexts[GetServers().GetIndex(from)] = ciphertext;
    _server_state->current_phase_log->server_messages[from] = ciphertext;

    qDebug() << GetServers().GetIndex(GetLocalId()) << GetLocalId().ToString() <<
      ": received ciphertext from" << GetServers().GetIndex(from) <<
      from.ToString() << "Have" << _server_state->handled_servers.count()
      << "expecting" << GetServers().Count();

    if(_server_state->handled_servers.count() == GetServers().Count()) {
      _state_machine.StateComplete();
    }
  }

  void CSDCNetRound::HandleServerValidation(const Connections::Id &from, QDataStream &stream)
  {
    if(!IsServer()) {
      throw QRunTimeError("Not a server");
    } else if(!GetServers().Contains(from)) {
      throw QRunTimeError("Not a server");
    }

    Q_ASSERT(_server_state);

    if(_server_state->handled_servers.contains(from)) {
      throw QRunTimeError("Already have signature.");
    }

    QByteArray signature;
    stream >> signature;

    if(!GetServers().GetKey(from)->
        Verify(_server_state->signed_hash, signature))
    {
      throw QRunTimeError("Signature doesn't match.");
    }

    _server_state->handled_servers.insert(from);
    _server_state->signatures[GetServers().GetIndex(from)] = signature;

    qDebug() << GetServers().GetIndex(GetLocalId()) << GetLocalId().ToString() <<
      ": received validation from" << GetServers().GetIndex(from) <<
      from.ToString() << "Have" << _server_state->handled_servers.count()
      << "expecting" << GetServers().Count();

    if(_server_state->handled_servers.count() == GetServers().Count()) {
      _state_machine.StateComplete();
    }
  }

  void CSDCNetRound::HandleBlameBits(const Connections::Id &from, QDataStream &stream)
  {
    if(!IsServer()) {
      throw QRunTimeError("Not a server");
    } else if(!GetServers().Contains(from)) {
      throw QRunTimeError("Not a server");
    }

    Q_ASSERT(_server_state);

    if(_server_state->blame_bits.contains(from)) {
      throw QRunTimeError("Already have blame bits.");
    }

    QPair<QBitArray, QBitArray> blame_bits;
    stream >> blame_bits;

    char expected =
      _server_state->phase_logs[_server_state->current_blame.third]->
      GetBitAtIndex(from, _server_state->current_blame.second);

    char actual = 0;
    for(int idx = 0; idx < blame_bits.first.size(); idx++) {
      actual ^= blame_bits.first[idx];
    }
    for(int idx = 0; idx < blame_bits.second.size(); idx++) {
      actual ^= blame_bits.second[idx];
    }

    if(actual != expected) {
      throw QRunTimeError("Blame bits do not match what was sent");
    }

    _server_state->blame_bits[from] = blame_bits;

    qDebug() << GetServers().GetIndex(GetLocalId()) << GetLocalId().ToString() <<
      ": received blame bits from" << GetServers().GetIndex(from) <<
      from.ToString() << "Have" << _server_state->blame_bits.count()
      << "expecting" << GetServers().Count();

    if(_server_state->blame_bits.count() == GetServers().Count()) {
      _state_machine.StateComplete();
    }
  }

  void CSDCNetRound::HandleRebuttal(const Connections::Id &from, QDataStream &stream)
  {
    if(from != _server_state->expected_rebuttal) {
      throw QRunTimeError("Not expecting rebuttal from client");
    }

    QPair<int, QByteArray> rebuttal;
    stream >> rebuttal;
    if(rebuttal.first >= GetServers().Count()) {
      _server_state->bad_dude = from;
      qDebug() << "Invalid server selected:" << from;
    } else {
      Connections::Id server = GetServers().GetId(rebuttal.first);
      QByteArray shared_secret = Crypto::DiffieHellman::VerifySharedSecret(
          GetClients().GetIdentity(from).GetDhKey(),
          GetServers().GetIdentity(server).GetDhKey(),
          rebuttal.second);
      if(shared_secret.isEmpty()) {
        _server_state->bad_dude = from;
        qDebug() << "Invalid shared secret:" << from;
      } else if(rebuttal.first >= _server_state->server_bits.size()) {
        _server_state->bad_dude = from;
        qDebug() << "Invalid server claim:" << from;
      } else {
        Hash hashalgo;
        hashalgo.Update(shared_secret);

        QByteArray bphase(4, 0);
        Serialization::WriteInt(_server_state->current_blame.third, bphase, 0);
        hashalgo.Update(bphase);

        hashalgo.Update(GetNonce());
        QByteArray seed = hashalgo.ComputeHash();
        int accuse_idx = _server_state->current_blame.second;
        int byte_idx = accuse_idx / 8;
        int bit_idx = accuse_idx % 8;
        QByteArray tmp(byte_idx + 1, 0);
        CryptoRandom(seed).GenerateBlock(tmp);

        if(((tmp[byte_idx] & bit_masks[bit_idx % 8]) != 0) == _server_state->server_bits[rebuttal.first]) {
          _server_state->bad_dude = from;
          qDebug() << "Client misbehaves:" << from;
        } else {
          _server_state->bad_dude = server;
          qDebug() << "Server misbehaves:" << server;
        }
      }
    }
    _state_machine.StateComplete();
  }

  void CSDCNetRound::HandleVerdictSignature(const Connections::Id &from, QDataStream &stream)
  {
    if(!IsServer()) {
      throw QRunTimeError("Not a server");
    } else if(!GetServers().Contains(from)) {
      throw QRunTimeError("Not a server");
    }

    if(_server_state->verdict_signatures.contains(from)) {
      throw QRunTimeError("Already have signature.");
    }

    QByteArray signature;
    stream >> signature;

    if(!GetServers().GetKey(from)->Verify(_server_state->verdict_hash, signature)) {
      throw QRunTimeError("Signature doesn't match.");
    }

    _server_state->verdict_signatures[from] = signature;

    qDebug() << GetServers().GetIndex(GetLocalId()) << GetLocalId().ToString() <<
      ": received verdict signature from" << GetServers().GetIndex(from) <<
      from.ToString() << "Have" << _server_state->verdict_signatures.count()
      << "expecting" << GetServers().Count();

    if(_server_state->verdict_signatures.count() == GetServers().Count()) {
      _state_machine.StateComplete();
    }
  }

  void CSDCNetRound::HandleRebuttalOrVerdict(const Connections::Id &from, QDataStream &stream)
  {
    if(IsServer()) {
      throw QRunTimeError("Not a client");
    } else if(!GetServers().Contains(from)) {
      throw QRunTimeError("Not a server");
    }

    bool verdict;
    stream >> verdict;
    if(!verdict) {
      int phase, accuse_idx;
      QBitArray server_bits;
      stream >> phase >> accuse_idx >> server_bits;

      QByteArray output;
      QDataStream ostream(&output, QIODevice::WriteOnly);
      ostream << CLIENT_REBUTTAL << GetNonce() << _state_machine.GetPhase() <<
        GetRebuttal(phase, accuse_idx, server_bits);
      VerifiableSend(from, output);
      return;
    }

    Utils::Triple<int, int, int> blame;
    Connections::Id bad_dude;
    QVector<QByteArray> signatures;
    stream >> blame >> bad_dude >> signatures;

    QByteArray verdict_msg;
    QDataStream vstream(&verdict_msg, QIODevice::WriteOnly);
    vstream << blame << bad_dude;

    QByteArray verdict_hash = Hash().ComputeHash(verdict_msg);

    int idx = 0;
    foreach(const PublicIdentity &pid, GetServers()) {
      if(!pid.GetKey()->Verify(verdict_hash, signatures[idx++])) {
        throw QRunTimeError("Invalid verdict signature");
      }
    }

    qDebug() << "Client done, bad guy:" << bad_dude;
    SetSuccessful(false);
    QVector<Connections::Id> bad_members;
    bad_members.append(bad_dude);
    SetBadMembers(bad_members);
    Stop("Bad member found and reported");
  }

  void CSDCNetRound::StartShuffle()
  {
#ifdef CS_BLOG_DROP
    _state->blame_shuffle->Start();
#else
    GetShuffleRound()->Start();
#endif
  }

  QPair<QByteArray, bool> CSDCNetRound::GetShuffleData(int)
  {
    QSharedPointer<Crypto::AsymmetricKey> key(new Crypto::DsaPrivateKey());
    _state->anonymous_key = key;

    QSharedPointer<Crypto::AsymmetricKey> pkey =
      QSharedPointer<Crypto::AsymmetricKey>(key->GetPublicKey());
    _state->shuffle_data = pkey->GetByteArray();

    return QPair<QByteArray, bool>(_state->shuffle_data, false);
  }

  QPair<QByteArray, bool> CSDCNetRound::GetBlameData(int)
  {
    if(!_state->my_accuse) {
      return QPair<QByteArray, bool>(QByteArray(), false);
    }

    qDebug() << GetLocalId() << "writing blame data";
    QByteArray msg(12, 0);
    Serialization::WriteUInt(_state->my_idx, msg, 0);
    Serialization::WriteUInt(_state->accuse_idx, msg, 4);
    Serialization::WriteUInt(_state->blame_phase, msg, 8);
    QByteArray signature = _state->anonymous_key->Sign(msg);
    msg.append(signature);

    return QPair<QByteArray, bool>(msg, false);
  }

  void CSDCNetRound::ShuffleFinished()
  {
    if(!GetShuffleRound()->Successful()) {
      SetBadMembers(GetShuffleRound()->GetBadMembers());
      if(GetShuffleRound()->Interrupted()) {
        SetInterrupted();
      }
      Stop("ShuffleRound failed");
      return;
    }

    _state_machine.StateComplete();
  }

  void CSDCNetRound::ProcessDataShuffle()
  {
    if(GetShuffleSink().Count() != GetClients().Count()) {
      qFatal("Did not receive a descriptor from everyone, expected: %d, found %d.",
          GetClients().Count(), GetShuffleSink().Count());
    }

    int count = GetShuffleSink().Count();
    for(int idx = 0; idx < count; idx++) {
      QPair<QSharedPointer<ISender>, QByteArray> pair(GetShuffleSink().At(idx));
      QSharedPointer<Crypto::AsymmetricKey> key(new Crypto::DsaPublicKey(pair.second));

      if(!key->IsValid()) {
        qDebug() << "Invalid key in shuffle.";
        continue;
      }

      if(_state->shuffle_data == pair.second) {
        _state->my_idx = idx;
      }
      _state->anonymous_keys.append(key);
    }

    if(!IsServer()) {
      Q_ASSERT(_state->anonymous_key);
      Q_ASSERT(_state->my_idx > -1);
      Q_ASSERT(_state->my_idx < _state->anonymous_keys.count());
    }

    _state_machine.StateComplete();
  }

  void CSDCNetRound::ProcessKeyShuffle()
  {
    QSharedPointer<NeffKeyShuffleRound> nks =
      GetShuffleRound().dynamicCast<NeffKeyShuffleRound>();
    Q_ASSERT(nks);

    _state->anonymous_keys = nks->GetKeys();

    if(!IsServer()) {
      _state->anonymous_key = nks->GetKey();
      Q_ASSERT(_state->anonymous_key);

      _state->my_idx = nks->GetKeyIndex();
      Q_ASSERT(_state->my_idx > -1);
      Q_ASSERT(_state->my_idx < _state->anonymous_keys.count());
    }

    _state_machine.StateComplete();
  }

#ifdef CS_BLOG_DROP
  void CSDCNetRound::ProcessBlogDrop()
  {
    QSharedPointer<BlogDropRound> bdr =
      _state->blame_shuffle.dynamicCast<BlogDropRound>();
    _state->anonymous_key = bdr->GetKey();
    Q_ASSERT(_state->anonymous_key);

    _state->anonymous_keys = bdr->GetKeys();

    _state->my_idx = -1;
    for(int idx = 0; idx < _state->anonymous_keys.count(); idx++) {
      if(_state->anonymous_key->VerifyKey(*_state->anonymous_keys[idx])) {
        _state->my_idx = idx;
        break;
      }
    }

    Q_ASSERT(_state->my_idx > -1);
    Q_ASSERT(_state->my_idx < _state->anonymous_keys.count());

    _state_machine.StateComplete();
  }
#endif

  void CSDCNetRound::PrepareForBulk()
  {
    _state->msg_length = (GetClients().Count() / 8);
    if(GetClients().Count() % 8) {
      ++_state->msg_length;
    }
    _state->base_msg_length = _state->msg_length;

    SetupRngSeeds();
    _state_machine.StateComplete();
    Utils::PrintResourceUsage(ToString() + " " + "beginning bulk");
  }

  void CSDCNetRound::SetupRngSeeds()
  {
    Identity::Roster roster;
    if(IsServer()) {
      roster = GetClients();
    } else {
      roster = GetServers();
    }

    foreach(const PublicIdentity &gc, roster) {
      if(gc.GetId() == GetLocalId()) {
        _state->base_seeds.append(QByteArray());
        continue;
      }
      QByteArray base_seed =
        GetPrivateIdentity().GetDhKey().GetSharedSecret(gc.GetDhKey());
      _state->base_seeds.append(base_seed);
    }
  }

  void CSDCNetRound::SetupRngs()
  {
    Hash hashalgo;

    QByteArray phase(4, 0);
    Serialization::WriteInt(_state_machine.GetPhase(), phase, 0);

    _state->anonymous_rngs.clear();

    QList<QByteArray> seeds = _state->base_seeds;
    if(IsServer()) {
      seeds = QList<QByteArray>();
      _server_state->rng_to_gidx.clear();
      for(int idx = 0; idx < _server_state->handled_clients.size(); idx++) {
        if(!_server_state->handled_clients.at(idx)) {
          continue;
        }
        _server_state->rng_to_gidx[seeds.size()] = idx;
        seeds.append(_state->base_seeds[idx]);
      }
    }

    foreach(const QByteArray &base_seed, seeds) {
      if(base_seed.isEmpty()) {
        continue;
      }
      hashalgo.Update(base_seed);
      hashalgo.Update(phase);
      hashalgo.Update(GetNonce());
      _state->anonymous_rngs.append(CryptoRandom(hashalgo.ComputeHash()));
    }
  }

  void CSDCNetRound::SubmitClientCiphertext()
  {
    SetupRngs();

    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << CLIENT_CIPHERTEXT << GetNonce() << _state_machine.GetPhase()
      << GenerateCiphertext();

    VerifiableSend(_state->my_server, payload);
  }

  QByteArray CSDCNetRound::GenerateCiphertext()
  {
    QByteArray xor_msg(_state->msg_length, 0);
    QByteArray tmsg(_state->msg_length, 0);
    
    int idx = 0;
    for(int jdx = 0; jdx < _state->anonymous_rngs.size(); jdx++) {
      _state->anonymous_rngs[jdx].GenerateBlock(tmsg);
      if(IsServer()) {
        int gidx = _server_state->rng_to_gidx[idx++];
        _server_state->current_phase_log->my_sub_ciphertexts[gidx] = tmsg;
      }
      Xor(xor_msg, xor_msg, tmsg);
    }

    if(_state->slot_open) {
      int offset = _state->base_msg_length;
      foreach(int owner, _state->next_messages.keys()) {
        if(owner == _state->my_idx) {
          break;
        }
        offset += _state->next_messages[owner];
      }

      QByteArray my_msg = GenerateSlotMessage();
      QByteArray my_xor_base = QByteArray::fromRawData(xor_msg.constData() +
          offset, my_msg.size());
      Xor(my_msg, my_msg, my_xor_base);
      xor_msg.replace(offset, my_msg.size(), my_msg);

      qDebug() << "Writing ciphertext into my slot" << _state->my_idx <<
        "starting at" << offset << "for" << my_msg.size() << "bytes.";

    } else if(CheckData()) {
      qDebug() << "Opening my slot" << _state->my_idx;
      xor_msg[_state->my_idx / 8] = xor_msg[_state->my_idx / 8] ^
        bit_masks[_state->my_idx % 8];
      _state->read = false;
      _state->slot_open = true;
    }

#ifdef BAD_CS_BULK
    if(xor_msg.size() == GetState()->base_msg_length) {
      qDebug() << "No damage done";
    } else {
      int offset = Random::GetInstance().GetInt(GetState()->base_msg_length + 1, xor_msg.size());
      xor_msg[offset] = xor_msg[offset] ^ 0xff;
      qDebug() << "up to no good";
    }
#endif
    return xor_msg;
  }

  bool CSDCNetRound::CheckData()
  {
    if(!_state->next_msg.isEmpty()) {
      return true;
    }

    QPair<QByteArray, bool> pair = GetData(MAX_GET);
    if(pair.first.size() > 0) {
      qDebug() << "Found a message of" << pair.first.size();
    }
    _state->next_msg = pair.first;
    _state->last_msg = QByteArray();
    return !_state->next_msg.isEmpty();
  }

  QByteArray CSDCNetRound::GenerateSlotMessage()
  {
    QByteArray msg = _state->next_msg;
    if(_state->read) {
      QPair<QByteArray, bool> pair = GetData(MAX_GET);
      _state->last_msg = _state->next_msg;
      _state->next_msg = pair.first;
    } else {
      msg = _state->last_msg;
      _state->read = !_state->accuse;
    }

    QByteArray msg_p(9, 0);

    if(_state->accuse) {
      msg_p[0] = 0xFF;
    }

    Serialization::WriteInt(_state_machine.GetPhase(), msg_p, 1);
    int length = _state->next_msg.size() + SlotHeaderLength(_state->my_idx);
#ifdef CSBR_CLOSE_SLOT
    if(_state->next_msg.size() == 0) {
      _state->slot_open = false;
      length = 0;
    }
#endif
    if(_state->accuse) {
      Serialization::WriteInt(SlotHeaderLength(_state->my_idx), msg_p, 5);
      msg_p.append(QByteArray(msg.size(), 0));
    } else {
      Serialization::WriteInt(length, msg_p, 5);
      msg_p.append(msg);
    }
#ifdef CSBR_SIGN_SLOTS
    QByteArray sig = _state->anonymous_key->Sign(msg_p);
#else
    QByteArray sig = Hash().ComputeHash(msg_p);
#endif

    QByteArray msg_pp = msg_p + sig;
    _state->last_ciphertext  = Randomize(msg_pp);
    return _state->last_ciphertext;
  }

  void CSDCNetRound::SetOnlineClients()
  {
#ifdef CSBR_RECONNECTS
    _server_state->allowed_clients.clear();

    foreach(const QSharedPointer<Connections::Connection> &con,
        GetOverlay()->GetConnectionTable().GetConnections())
    {
      if(GetOverlay()->IsServer(con->GetRemoteId())) {
        continue;
      }

      _server_state->allowed_clients.insert(con->GetRemoteId());
    }
#endif

    if(_server_state->allowed_clients.count() == 0) {
      _state_machine.StateComplete();
      return;
    }

    // This is the hard deadline
    Utils::TimerCallback *cb = new Utils::TimerMethod<CSDCNetRound, int>(
        this, &CSDCNetRound::ConcludeClientCiphertextSubmission, 0);
    _server_state->client_ciphertext_period =
      Utils::Timer::GetInstance().QueueCallback(cb, CLIENT_SUBMISSION_WINDOW);

    // Setup the flex-deadline
    _server_state->start_of_phase =
      Utils::Time::GetInstance().MSecsSinceEpoch();
    _server_state->expected_clients =
      int(_server_state->allowed_clients.count() * CLIENT_PERCENTAGE);
  }

  void CSDCNetRound::ConcludeClientCiphertextSubmission(const int &)
  {
    qDebug() << "Client window has closed, unfortunately some client may not"
      << "have transmitted in time.";
    _state_machine.StateComplete();
  }

  void CSDCNetRound::SubmitClientList()
  {
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << SERVER_CLIENT_LIST << GetNonce() <<
      _state_machine.GetPhase() << _server_state->handled_clients;

    VerifiableBroadcastToServers(payload);
  }

  void CSDCNetRound::SubmitCommit()
  {
    SetupRngs();

    qDebug() << ToString() << "generating ciphertext for" <<
      _state->anonymous_rngs.count() << "out of" << GetClients().Count();

    GenerateServerCiphertext();

    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << SERVER_COMMIT << GetNonce() <<
      _state_machine.GetPhase() << _server_state->my_commit;

    VerifiableBroadcastToServers(payload);
  }

  void CSDCNetRound::GenerateServerCiphertext()
  {
    QByteArray ciphertext = GenerateCiphertext();
    for(int lidx = 0; lidx < _server_state->client_ciphertexts.size(); lidx++) {
      const QPair<int, QByteArray> &entry = _server_state->client_ciphertexts[lidx];
      int idx = entry.first;
      const QByteArray &text = entry.second;

      if(!_server_state->handled_clients.at(idx)) {
        continue;
      }
      Xor(ciphertext, ciphertext, text);
    }

    QBitArray open(GetClients().Count(), false);
    for(int idx = 0; idx < _state->next_messages.size(); idx++) {
      open[idx] = _state->next_messages[idx] != 0;
    }

    _server_state->my_ciphertext = ciphertext;
    _server_state->my_commit = Hash().ComputeHash(ciphertext);
  }

  void CSDCNetRound::SubmitServerCiphertext()
  {
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << SERVER_CIPHERTEXT << GetNonce() <<
      _state_machine.GetPhase() << _server_state->my_ciphertext;

    VerifiableBroadcastToServers(payload);
  }

  void CSDCNetRound::SubmitValidation()
  {
    QByteArray cleartext(_state->msg_length, 0);

    foreach(const QByteArray &ciphertext, _server_state->server_ciphertexts) {
      Xor(cleartext, cleartext, ciphertext);
    }

    _state->cleartext = cleartext;
    Hash hash;
    hash.Update(_state->cleartext);

    QByteArray data;
    QDataStream tstream(&data, QIODevice::WriteOnly);
    /** XXX servers are currently assumed to be always online and 
     * and are allocated a slot
     */
    tstream << _server_state->handled_clients;
    hash.Update(data);

    _server_state->signed_hash = hash.ComputeHash();
    QByteArray signature = GetKey()->Sign(_server_state->signed_hash);

    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << SERVER_VALIDATION << GetNonce() <<
      _state_machine.GetPhase() << signature;

    VerifiableBroadcastToServers(payload);
  }

  void CSDCNetRound::PushCleartext()
  {
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << SERVER_CLEARTEXT << GetNonce() << _state_machine.GetPhase()
      << _server_state->signatures << _server_state->cleartext <<
      _server_state->handled_clients;

    VerifiableBroadcastToClients(payload);
    ProcessCleartext();
    if(_state->start_accuse) {
      _state_machine.SetState(STARTING_BLAME_SHUFFLE);
    } else {
      _state_machine.StateComplete();
    }
  }

  void CSDCNetRound::StartBlameShuffle()
  {
#ifdef CS_BLOG_DROP
    _state->blame_shuffle.dynamicCast<BlogDropRound>()->Resume(_state->accuser);
#else
    _state->blame_shuffle->Start();
#endif
  }

  void CSDCNetRound::ProcessBlameShuffle()
  {
    if(!IsServer()) {
      _state_machine.StateComplete();
      return;
    }

    for(int idx = 0; idx < _blame_sink.Count(); idx++) {
      const QByteArray &blame = _blame_sink.At(idx).second;
      if(blame.size() <= 8) {
        qDebug() << "Found invalid blame material";
        continue;
      }

      QByteArray msg = blame.left(12);
      QByteArray signature = blame.mid(12);
      int owner_idx = Serialization::ReadInt(msg, 0);
      int accuse_idx = Serialization::ReadInt(msg, 4);
      int accuse_bidx = (accuse_idx / 8) + (accuse_idx % 8 ? 1 : 0);
      int phase = Serialization::ReadInt(msg, 8);
      if(!_server_state->phase_logs.contains(phase)) {
        qDebug() << "Phase too old" << phase;
        continue;
      }

      if(owner_idx < 0 || owner_idx >= _state->anonymous_keys.size()) {
        qDebug() << "Invalid idx claimed";
        continue;
      }

      QSharedPointer<PhaseLog> phase_log = _server_state->phase_logs[phase];
      int start = phase_log->message_offsets[owner_idx];
      int end = (owner_idx + 1 == phase_log->message_offsets.size()) ?
        phase_log->message_length :
        phase_log->message_offsets[owner_idx + 1];

      if((end - start + accuse_bidx) < 0) {
        qDebug() << "Invalid offset claimed:" << (end - start + accuse_idx);
        continue;
      }
      
      if(!_state->anonymous_keys[owner_idx]->Verify(msg, signature)) {
        qDebug() << "Invalid accusation" <<  owner_idx << signature.size() << signature.toBase64();
        continue;
      }

      qDebug() << "Found a valid accusation for" << owner_idx << accuse_idx << phase;
      if(!_server_state->accuse_found) {
        _server_state->current_blame =
          Utils::Triple<int, int, int>(owner_idx, accuse_idx, phase);
        _server_state->accuse_found = true;
      }
    }

    if(_server_state->accuse_found) {
      _state_machine.StateComplete();
    } else {
      throw QRunTimeError("Missing accusation");
    }
  }

  void CSDCNetRound::TransmitBlameBits()
  {
    QPair<QBitArray, QBitArray> bits =
      _server_state->phase_logs[_server_state->current_blame.third]->
      GetBitsAtIndex(_server_state->current_blame.second);

    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << SERVER_BLAME_BITS << GetNonce() <<
      _state_machine.GetPhase() << bits;
    VerifiableBroadcastToServers(payload);
    _state_machine.StateComplete();
  }

  void CSDCNetRound::RequestRebuttal()
  {
    QPair<int, QBitArray> pair = FindMismatch();
    int gidx = pair.first;
    if(gidx == -1) {
      qDebug() << "Did not find a mismatch";
      return;
    }

    // XXX At this point, we should ask the server who received the ciphertext
    // to transmit it to the group.
    // If it doesn't match what the server sent, he equivocates.

    QBitArray server_bits = pair.second;
    Connections::Id id = GetClients().GetId(gidx);
    _server_state->expected_rebuttal = id;
    _server_state->server_bits = server_bits;

    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    int accuse_idx = _server_state->current_blame.second;
    int phase = _server_state->current_blame.third;
    stream << SERVER_REBUTTAL_OR_VERDICT << GetNonce() <<
      _state_machine.GetPhase() << false <<
      phase << accuse_idx << server_bits;
    VerifiableSend(id, payload);
    _state_machine.StateComplete();
  }

  void CSDCNetRound::SubmitVerdictSignature()
  {
    QByteArray verdict;
    QDataStream vstream(&verdict, QIODevice::WriteOnly);
    vstream << _server_state->current_blame << _server_state->bad_dude;

    _server_state->verdict_hash = Hash().ComputeHash(verdict);
    QByteArray signature = GetKey()->Sign(_server_state->verdict_hash);

    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << SERVER_VERDICT_SIGNATURE << GetNonce() <<
      _state_machine.GetPhase() << signature;
    VerifiableBroadcastToServers(payload);
    _state_machine.StateComplete();
  }

  void CSDCNetRound::PushVerdict()
  {
    QVector<QByteArray> signatures;
    foreach(const PublicIdentity &pid, GetServers()) {
      signatures.append(_server_state->verdict_signatures[pid.GetId()]);
    }

    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << SERVER_REBUTTAL_OR_VERDICT << GetNonce() <<
      _state_machine.GetPhase() << true <<
      _server_state->current_blame <<
      _server_state->bad_dude << signatures;
    VerifiableBroadcastToClients(payload);

    SetSuccessful(false);
    QVector<Connections::Id> bad_members;
    bad_members.append(_server_state->bad_dude);
    SetBadMembers(bad_members);
    Stop("Bad member found and reported");
  }

  void CSDCNetRound::ProcessCleartext()
  {
    int next_msg_length = _state->base_msg_length;
    QMap<int, int> next_msgs;
    for(int idx = 0; idx < GetClients().Count(); idx++) {
      if(_state->cleartext[idx / 8] & bit_masks[idx % 8]) {
        int length = SlotHeaderLength(idx);
        next_msgs[idx] = length;
        next_msg_length += length;
        qDebug() << "Opening slot" << idx;
      }
    }

    int offset = GetClients().Count() / 8;
    if(GetClients().Count() % 8) {
      ++offset;
    }

#ifndef CSBR_SIGN_SLOTS
    Hash hashalgo;
    int sig_length = hashalgo.GetDigestSize();
#endif

    if(IsServer()) {
      int calc = offset;
      for(int idx = 0; idx < GetClients().Count(); idx++) {
        _server_state->current_phase_log->message_offsets.append(calc);
        int msg_length = _state->next_messages.contains(idx) ?
          _state->next_messages[idx] : 0;
        calc += msg_length;
      }
    }

    foreach(int owner, _state->next_messages.keys()) {
      int msg_length = _state->next_messages[owner];
      if(msg_length == 0) {
        continue;
      }

      QByteArray msg_ppp = QByteArray::fromRawData(
          _state->cleartext.constData() + offset, msg_length);
      offset += msg_length;

      QByteArray msg_pp = Derandomize(msg_ppp);
      if(msg_pp.isEmpty()) {
        qDebug() << "No message at" << owner;
        next_msg_length += msg_length;
        next_msgs[owner] = msg_length;

        if(_state->my_idx == owner) {
          _state->read = false;
          _state->slot_open = true;
          qDebug() << "My message didn't make it in time.";
        }
        continue;
      }

#ifdef CSBR_SIGN_SLOTS
      QSharedPointer<Crypto::AsymmetricKey> vkey(_state->anonymous_keys[owner]);
      int sig_length = vkey->GetSignatureLength();
#endif

      QByteArray msg_p = QByteArray::fromRawData(
          msg_pp.constData(), msg_pp.size() - sig_length);
      QByteArray sig = QByteArray::fromRawData(
          msg_p.constData() + msg_p.size(), sig_length);

      bool bad_message = false;
#ifdef CSBR_SIGN_SLOTS
      if(!vkey->Verify(msg_pp, sig)) {
#else
      if(hashalgo.ComputeHash(msg_p) != sig) {
#endif
        qDebug() << "Unable to verify message for peer at" << owner;
        next_msg_length += msg_length;
        next_msgs[owner] = msg_length;

        if(owner == _state->my_idx && !_state->accuse) {
          _state->read = false;
          _state->slot_open = true;
          for(int pidx = 0; pidx < msg_ppp.size(); pidx++) {
            const char expected = _state->last_ciphertext[pidx];
            const char actual = msg_ppp[pidx];
            if(expected == actual) {
              continue;
            }
            for(int bidx = 0; bidx < 8; bidx++) {
              const char expected_bit = expected & bit_masks[bidx];
              const char actual_bit = actual & bit_masks[bidx];
              if(actual_bit == expected_bit) {
                continue;
              }

              if(expected_bit != 0) {
                qDebug() << "Bit flipped, but expected bit isn't 0";
                continue;
              }
              _state->accuse_idx = (offset - msg_length + pidx) * 8 + bidx;
              _state->accuse = true;
              _state->blame_phase = _state_machine.GetPhase();
              break;
            }

            if(_state->accuse) {
              break;
            }
          }
          if(_state->accuse) {
            qDebug() << "My message got corrupted, blaming" <<
              _state->accuse_idx << _state->blame_phase;
          } else {
            qDebug() << msg_ppp.toBase64() << msg_ppp.size() << msg_length;
            qDebug() << _state->last_ciphertext.toBase64() << _state->last_ciphertext.size();
            qDebug() << "My message got corrupted cannot blame";
          }
        }
        bad_message = true;
      }

      if(msg_p[0] != char(0)) {
        _state->start_accuse = true;
        _state->accuser = owner;
        if(owner == _state->my_idx) {
          // Only submit an accusation if we have one...
          _state->my_accuse = _state->accuse;
        }
        qDebug() << "Accusation generated by" << owner;
      }

      if(bad_message) {
        continue;
      }

      int phase = Serialization::ReadInt(msg_p, 1);
      if(phase != _state_machine.GetPhase()) {
        next_msg_length += msg_length;
        next_msgs[owner] = msg_length;
        qDebug() << "Incorrect phase, skipping message";
        continue;
      }

      int next = Serialization::ReadInt(msg_p, 5);
      if(next < 0) {
        next_msg_length += msg_length;
        next_msgs[owner] = msg_length;
        qDebug() << "Invalid next message size, skipping message";
        continue;
      } else if(next > 0) {
        qDebug() << "Slot" << owner << "next message length:" << next;
        next_msgs[owner] = next;
        next_msg_length += next;
      } else {
        qDebug() << "Slot" << owner << "closing";
      }

      QByteArray msg(msg_p.constData() + 9, msg_p.size() - 9);
      if(!msg.isEmpty()) {
        qDebug() << ToString() << "received a valid message.";
        PushData(owner, msg);
      }
    }

    if(IsServer()) {
      _server_state->current_phase_log->message_length = offset;
    }

    _state->next_messages = next_msgs;
    _state->msg_length = next_msg_length;
  }

  QByteArray CSDCNetRound::NullSeed()
  {
    static QByteArray null_seed(CryptoRandom::OptimalSeedSize(), 0);
    return null_seed;
  }

  QByteArray CSDCNetRound::Randomize(const QByteArray &msg)
  {
    CryptoRandom rand;
    QByteArray seed(CryptoRandom::OptimalSeedSize(), 0);
    do {
      rand.GenerateBlock(seed);
    } while(seed == NullSeed());

    QByteArray random_text(msg.size(), 0);
    CryptoRandom(seed).GenerateBlock(random_text);

    Xor(random_text, random_text, msg);

    return seed + random_text;
  }

  QByteArray CSDCNetRound::Derandomize(const QByteArray &randomized_text)
  {
    QByteArray seed = QByteArray::fromRawData(randomized_text.constData(),
        CryptoRandom::OptimalSeedSize());

    if(seed == NullSeed()) {
      return QByteArray();
    }

    QByteArray msg = QByteArray::fromRawData(
        randomized_text.constData() + seed.size(),
        randomized_text.size() - seed.size());

    QByteArray random_text(msg.size(), 0);
    CryptoRandom(seed).GenerateBlock(random_text);

    Xor(random_text, random_text, msg);
    return random_text;
  }

  QPair<int, QBitArray> CSDCNetRound::FindMismatch()
  {
    QBitArray actual(GetServers().Count(), false);
    QBitArray expected(GetServers().Count(), false);
    foreach(const Connections::Id &key, _server_state->blame_bits.keys()) {
      const QPair<QBitArray, QBitArray> &pair = _server_state->blame_bits[key];
      actual ^= pair.first;
      expected ^= pair.second;
    }

    if(actual == expected) {
      throw QRunTimeError("False accusation");
    }
    QBitArray mismatch = (actual ^ expected);
    bool first_found = false;
    int first = -1;
    for(int idx = 0; idx < mismatch.size(); idx++) {
      if(mismatch.at(idx)) {
        qDebug() << "Found a mismatch at" << idx;
        if(!first_found) {
          first_found = true;
          first = idx;
        }
      }
    }

    if(!first_found) {
      return QPair<int, QBitArray>(-1, QBitArray());
    }

    QBitArray server_bits(_server_state->blame_bits.size(), false);
    int idx = 0;
    foreach(const PublicIdentity &pid, GetServers()) {
      const QPair<QBitArray, QBitArray> &pair = _server_state->blame_bits[pid.GetId()];
      server_bits[idx++] = pair.second.at(first);
    }

    return QPair<int, QBitArray>(first, server_bits);
  }

  QPair<int, QByteArray> CSDCNetRound::GetRebuttal(int phase, int accuse_idx,
      const QBitArray &server_bits)
  {
    Hash hashalgo;

    QByteArray bphase(4, 0);
    Serialization::WriteInt(phase, bphase, 0);

    int byte_idx = accuse_idx / 8;
    int bit_idx = accuse_idx % 8;
    int msg_size = byte_idx + 1;

    int bidx = -1;
    QByteArray tmp(msg_size, 0);

    for(int idx = 0; idx < _state->base_seeds.size(); idx++) {
      const QByteArray &base_seed = _state->base_seeds[idx];
      hashalgo.Update(base_seed);
      hashalgo.Update(bphase);
      hashalgo.Update(GetNonce());
      CryptoRandom(hashalgo.ComputeHash()).GenerateBlock(tmp);
      if(((tmp[byte_idx] & bit_masks[bit_idx]) != 0) != server_bits[idx]) {
        bidx = idx;
        break;
      }
    }

    if(bidx >= 0) {
      qDebug() << "Found the mismatch!" << bidx;
    } else {
      bidx = phase % GetServers().Count();
      qDebug() << "We gotz busted, blaming" << bidx;
    }

    Connections::Id bid = GetServers().GetId(bidx);
    QByteArray server_dh = GetServers().GetIdentity(bid).GetDhKey();
    QByteArray proof = GetPrivateIdentity().GetDhKey().ProveSharedSecret(server_dh);
    return QPair<int, QByteArray>(bidx, proof);
  }
}
}
