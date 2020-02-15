#include "appproxy.h"

#include <algorithm>
#include <cstdlib>
#include <functional>

#include <QByteArray>
#include <QClipboard>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QPair>
#include <QPixmap>
#include <QQmlContext>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QScreen>
#include <QSettings>
#include <QSysInfo>
#include <QUrl>
#include <QUrlQuery>

#include "constants.h"
#include "networkproxy.h"
#include "networkrequest.h"
#include "qrcodehelper.h"

AppProxy::AppProxy(QObject* parent)
  : QObject(parent),
    v2ray(V2RayCore::getInstance()),
    configurator(Configurator::getInstance()) {
  // Setup Worker
  worker->moveToThread(&workerThread);
  connect(&workerThread, &QThread::finished, worker, &QObject::deleteLater);

  // Setup Worker -> getServerLatency
  connect(this, &AppProxy::getServerLatencyStarted, worker,
          &AppProxyWorker::getServerLatency);
  connect(worker, &AppProxyWorker::serverLatencyReady, this,
          &AppProxy::returnServerLatency);

  // Setup Worker -> getGfwList
  connect(this, &AppProxy::getGfwListStarted, worker,
          &AppProxyWorker::getGfwList);
  connect(worker, &AppProxyWorker::gfwListReady, this,
          &AppProxy::returnGfwList);

  // Setup Worker -> getNetworkStatus
  connect(this, &AppProxy::getNetworkStatusStarted, worker,
          &AppProxyWorker::getUrlAccessibility);
  connect(worker, &AppProxyWorker::urlAccessibilityReady, this,
          &AppProxy::returnNetworkAccessiblity);

  // Setup Worker -> getSubscriptionServers
  connect(this, &AppProxy::getSubscriptionServersStarted, worker,
          &AppProxyWorker::getSubscriptionServers);
  connect(worker, &AppProxyWorker::subscriptionServersReady, this,
          &AppProxy::addSubscriptionServers);

  // Setup Worker -> getLogs
  connect(this, &AppProxy::getLogsStarted, worker, &AppProxyWorker::getLogs);
  connect(worker, &AppProxyWorker::logsReady, this, &AppProxy::returnLogs);

  workerThread.start();
}

AppProxy::~AppProxy() {
  workerThread.quit();
  workerThread.wait();
}

void AppProxy::getAppVersion() {
  QString appVersion = QString("v%1.%2.%3")
                         .arg(QString::number(APP_VERSION_MAJOR),
                              QString::number(APP_VERSION_MINOR),
                              QString::number(APP_VERSION_PATCH));
  emit appVersionReady(appVersion);
}

void AppProxy::getV2RayCoreVersion() {
  QJsonObject appConfig    = configurator.getAppConfig();
  QString v2RayCoreVersion = appConfig["v2rayCoreVersion"].toString();
  emit v2RayCoreVersionReady(v2RayCoreVersion);
}

void AppProxy::getOperatingSystem() {
  QString operatingSystem = QSysInfo::prettyProductName();
  emit operatingSystemReady(operatingSystem);
}

void AppProxy::getV2RayCoreStatus() {
  emit v2RayCoreStatusReady(v2ray.isRunning());
}

void AppProxy::setV2RayCoreRunning(bool expectedRunning) {
  bool isSuccessful = false;
  if (expectedRunning) {
    isSuccessful = v2ray.start();
    qInfo() << QString("Start V2Ray Core ... %1")
                 .arg(isSuccessful ? "success" : "failed");
  } else {
    isSuccessful = v2ray.stop();
    qInfo() << QString("Stop V2Ray Core ... %1")
                 .arg(isSuccessful ? "success" : "failed");
  }
  if (isSuccessful) {
    emit v2RayCoreStatusReady(expectedRunning);
  }
}

void AppProxy::getNetworkStatus() {
  qRegisterMetaType<QMap<QString, bool>>("QMap");
  qRegisterMetaType<QNetworkProxy>("QNetworkProxy");
  emit getNetworkStatusStarted({{"google.com", true}, {"baidu.com", false}},
                               getQProxy());
}

QNetworkProxy AppProxy::getQProxy() {
  QJsonArray connectedServers = configurator.getConnectedServers();
  if (connectedServers.size() == 0) {
    return QNetworkProxy::NoProxy;
  }

  QJsonObject appConfig = configurator.getAppConfig();
  QNetworkProxy::ProxyType proxyType =
    appConfig["serverProtocol"].toString() == "SOCKS"
      ? QNetworkProxy::Socks5Proxy
      : QNetworkProxy::HttpProxy;
  int serverPort = appConfig["serverPort"].toInt();
  QNetworkProxy proxy;
  proxy.setType(proxyType);
  proxy.setHostName("127.0.0.1");
  proxy.setPort(serverPort);
  return proxy;
}

void AppProxy::returnNetworkAccessiblity(QMap<QString, bool> accessible) {
  bool isGoogleAccessible =
         accessible.contains("google.com") ? accessible["google.com"] : false,
       isBaiduAccessible =
         accessible.contains("baidu.com") ? accessible["baidu.com"] : false;

  emit networkStatusReady(
    QJsonDocument(QJsonObject{
                    {"isGoogleAccessible", isGoogleAccessible},
                    {"isBaiduAccessible", isBaiduAccessible},
                  })
      .toJson());
}

void AppProxy::getAppConfig() {
  QJsonObject appConfig = configurator.getAppConfig();
  emit appConfigReady(QJsonDocument(appConfig).toJson());
}

void AppProxy::setAppConfig(QString configString) {
  QJsonDocument configDoc = QJsonDocument::fromJson(configString.toUtf8());
  QJsonObject appConfig   = configDoc.object();
  // Check if app config contains errors
  QStringList appConfigErrors = getAppConfigErrors(appConfig);
  if (appConfigErrors.size() > 0) {
    emit appConfigError(appConfigErrors.join('\n'));
    return;
  }
  // Set auto start and update UI language
  setAutoStart(appConfig["autoStart"].toBool());
  retranslate(appConfig["language"].toString());
  // Save app config
  appConfig["serverPort"] = appConfig["serverPort"].toString().toInt();
  appConfig["pacPort"]    = appConfig["pacPort"].toString().toInt();
  configurator.setAppConfig(appConfig);
  qInfo() << "Application config updated. Restarting V2Ray ...";
  // Restart V2Ray Core
  v2ray.restart();
  // Notify that the app config has changed
  emit appConfigChanged();
}

QStringList AppProxy::getAppConfigErrors(const QJsonObject& appConfig) {
  QStringList errors;
  errors.append(getStringConfigError(appConfig, "language", tr("Language")));
  errors.append(getStringConfigError(appConfig, "serverProtocol",
                                     tr("Local Server Protocol")));
  errors.append(getStringConfigError(
    appConfig, "serverIp", tr("Listening IP Address"),
    {
      std::bind(&AppProxy::isIpAddrValid, this, std::placeholders::_1),
    }));
  errors.append(getNumericConfigError(appConfig, "serverPort",
                                      tr("Listening Port"), 1, 65535));
  errors.append(getNumericConfigError(appConfig, "pacPort",
                                      tr("PAC Server Port"), 1, 65535));
  if (appConfig["pacPort"].toString() == appConfig["serverPort"].toString()) {
    errors.append(
      tr("'PAC Server Port' can not be the same as 'Listening Port'."));
  }
  if (!appConfig.contains("dns") || appConfig["dns"].toString().isEmpty()) {
    errors.append(tr("Missing the value of 'DNS Servers'."));
  } else {
    QStringList dnsServers = appConfig["dns"].toString().split(",");
    for (QString dnsServer : dnsServers) {
      if (!isIpAddrValid(dnsServer.trimmed())) {
        errors.append(tr("'DNS Servers' seems invalid."));
        break;
      }
    }
  }
  // Remove empty error messages generated by getNumericConfigError() and
  // getStringConfigError() and getStringConfigError()
  errors.removeAll("");
  return errors;
}

bool AppProxy::retranslate(QString language) {
  if (language.isEmpty()) {
    Configurator& configurator(Configurator::getInstance());
    language = configurator.getLanguage();
  }
  QCoreApplication* app = QGuiApplication::instance();
  app->removeTranslator(&translator);
  bool isTrLoaded = translator.load(
    QString("%1/%2.qm").arg(Configurator::getLocaleDirPath(), language));

  app->installTranslator(&translator);
  QQmlEngine::contextForObject(this)->engine()->retranslate();
  return isTrLoaded;
}

void AppProxy::setAutoStart(bool autoStart) {
  const QString APP_NAME = "V2Ray Desktop";
  const QString APP_PATH =
    QDir::toNativeSeparators(QGuiApplication::applicationFilePath());
#if defined(Q_OS_WIN)
  QSettings settings(
    "HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
    QSettings::NativeFormat);
#elif defined(Q_OS_LINUX)
  QFile srcFile(":/misc/tpl-linux-autostart.desktop");
  QFile dstFile(QString("%1/.config/autostart/v2ray-dekstop.desktop")
                  .arg(QDir::homePath()));
#elif defined(Q_OS_MAC)
  QFile srcFile(":/misc/tpl-macos-autostart.plist");
  QFile dstFile(
    QString("%1/Library/LaunchAgents/com.v2ray.desktop.launcher.plist")
      .arg(QDir::homePath()));
#endif

#if defined(Q_OS_WIN)
  if (autoStart) {
    settings.setValue(APP_NAME, APP_PATH);
  } else {
    settings.remove(APP_NAME);
  }
#elif defined(Q_OS_LINUX) or defined(Q_OS_MAC)
  if (autoStart) {
    QString fileContent;
    if (srcFile.exists() &&
        srcFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
      fileContent = srcFile.readAll();
    }
    if (dstFile.open(QIODevice::WriteOnly)) {
      dstFile.write(fileContent.arg(APP_PATH).toUtf8());
      dstFile.close();
    }
  } else {
    if (dstFile.exists()) {
      dstFile.remove();
    }
  }
#endif
}

void AppProxy::getLogs() {
  emit getLogsStarted(Configurator::getAppLogFilePath(),
                      Configurator::getV2RayLogFilePath());
}

void AppProxy::returnLogs(QString logs) { emit logsReady(logs); }

void AppProxy::clearLogs() {
  QFile appLogFile(Configurator::getAppLogFilePath());
  QFile v2RayLogFile(Configurator::getV2RayLogFilePath());
  if (appLogFile.exists()) {
    appLogFile.resize(0);
  }
  if (v2RayLogFile.exists()) {
    v2RayLogFile.resize(0);
  }
}

void AppProxy::getProxySettings() {
  bool isV2RayRunning     = v2ray.isRunning();
  bool isPacServerRunning = pacServer.isRunning();

  QString proxyMode = NetworkProxyHelper::getSystemProxy().toString();
  QStringList connectedServers = configurator.getConnectedServerNames();
  emit proxySettingsReady(
    QJsonDocument(
      QJsonObject{{"isV2RayRunning", isV2RayRunning},
                  {"isPacServerRunning", isPacServerRunning},
                  {"proxyMode", proxyMode},
                  {"connectedServers", connectedServers.join(", ")}})
      .toJson());
}

void AppProxy::setSystemProxyMode(QString proxyMode) {
  QJsonObject appConfig = configurator.getAppConfig();
  // Automatically set system proxy according to app config
  if (!proxyMode.size()) {
    proxyMode = appConfig["proxyMode"].toString();
  }

  // Set system proxy
  NetworkProxy proxy;
  proxy.host = "127.0.0.1";
  NetworkProxyHelper::resetSystemProxy();
  if (pacServer.isRunning()) {
    pacServer.stop();
  }
  if (proxyMode == "global") {
    QString protocol = appConfig["serverProtocol"].toString();
    proxy.port       = appConfig["serverPort"].toInt();
    proxy.type       = protocol == "SOCKS" ? NetworkProxyType::SOCKS_PROXY
                                     : NetworkProxyType::HTTP_PROXY;
  } else if (proxyMode == "pac") {
    proxy.port = appConfig["pacPort"].toInt();
    proxy.type = NetworkProxyType::PAC_PROXY;
    proxy.url  = QString("http://%1:%2/proxy.pac")
                  .arg(proxy.host, QString::number(proxy.port));
    // Restart PAC Server
    QString pacServerHost = appConfig["serverIp"].toString();
    pacServer.start(pacServerHost, proxy.port);
  }
  NetworkProxyHelper::setSystemProxy(proxy);
  emit proxyModeChanged(proxyMode);

  // Update app config
  configurator.setAppConfig({{"proxyMode", proxyMode}});
}

void AppProxy::setGfwListUrl(QString gfwListUrl) {
  QJsonObject appConfig = {{"gfwListUrl", gfwListUrl}};
  QString error         = getStringConfigError(
    appConfig, "gfwListUrl", tr("GFW List URL"),
    {std::bind(&AppProxy::isUrlValid, this, std::placeholders::_1)});
  if (!error.isEmpty()) {
    emit appConfigError(error);
    return;
  }
  configurator.setAppConfig(appConfig);
  emit appConfigChanged();
}

void AppProxy::updateGfwList(QString gfwListUrl) {
  emit getGfwListStarted(gfwListUrl, getQProxy());
}

void AppProxy::returnGfwList(QByteArray gfwList) {
  if (gfwList.size()) {
    QFile gfwListFile(Configurator::getGfwListFilePath());
    gfwListFile.open(QFile::WriteOnly);
    gfwListFile.write(gfwList);
    gfwListFile.flush();
    // Update app config
    QString updatedTime = QDateTime::currentDateTime().toString();
    configurator.setAppConfig(QJsonObject{
      {"gfwListLastUpdated", QDateTime::currentDateTime().toString()}});
    qInfo() << "GFW List updated successfully.";
    emit gfwListUpdated(updatedTime);
  } else {
    emit gfwListUpdated(tr("Failed to update GFW List."));
  }
}

void AppProxy::getServers() {
  QJsonArray servers               = configurator.getServers();
  QStringList connectedServerNames = configurator.getConnectedServerNames();

  for (auto itr = servers.begin(); itr != servers.end(); ++itr) {
    QJsonObject server = (*itr).toObject();
    QString serverName =
      server.contains("serverName") ? server["serverName"].toString() : "";
    server["connected"] = connectedServerNames.contains(serverName);
    if (serverLatency.contains(serverName)) {
      server["latency"] = serverLatency[serverName].toInt();
    }
    *itr = server;
  }
  emit serversReady(QJsonDocument(servers).toJson());
}

void AppProxy::getServer(QString serverName, bool forDuplicate) {
  QJsonObject server = configurator.getServer(serverName);
  if (forDuplicate) {
    server.remove("serverName");
  }
  emit serverDInfoReady(QJsonDocument(server).toJson());
}

void AppProxy::getServerLatency(QString serverName) {
  QJsonObject _serverLatency;
  QJsonArray servers;
  if (serverName.size()) {
    servers.append(configurator.getServer(serverName));
  } else {
    servers = configurator.getServers();
  }
  qRegisterMetaType<QJsonArray>("QJsonArray");
  emit getServerLatencyStarted(servers);
}

void AppProxy::returnServerLatency(QMap<QString, QVariant> latency) {
  // Ref:
  // https://stackoverflow.com/questions/8517853/iterating-over-a-qmap-with-for
  // Note: Convert to StdMap for better performance
  for (auto l : latency.toStdMap()) {
    serverLatency[l.first] = l.second.toInt();
  }
  emit serverLatencyReady(QJsonDocument::fromVariant(latency).toJson());
}

void AppProxy::setServerConnection(QString serverName, bool connected) {
  configurator.setServerConnection(serverName, connected);
  v2ray.restart();
  qInfo() << (connected ? "Connected to " : "Disconnected from ") << serverName;
  emit serverConnectivityChanged(serverName, connected);
}

void AppProxy::addV2RayServer(QString configString) {
  QJsonDocument configDoc  = QJsonDocument::fromJson(configString.toUtf8());
  QJsonObject serverConfig = configDoc.object();
  // Check server config before saving
  QStringList serverConfigErrors = getV2RayServerConfigErrors(serverConfig);
  if (serverConfigErrors.size() > 0) {
    emit serverConfigError(serverConfigErrors.join('\n'));
    return;
  }
  // Save server config
  configurator.addServer(getPrettyV2RayConfig(serverConfig));
  emit serversChanged();
  qInfo() << QString("Add new V2Ray server [Name=%1, Addr=%2].")
               .arg(serverConfig["serverName"].toString(),
                    serverConfig["serverAddr"].toString());
}

QStringList AppProxy::getV2RayServerConfigErrors(
  const QJsonObject& serverConfig) {
  QStringList errors;
  errors.append(
    getStringConfigError(serverConfig, "serverName", tr("Server Name")));
  errors.append(getStringConfigError(
    serverConfig, "serverAddr", tr("Server Address"),
    {
      std::bind(&AppProxy::isIpAddrValid, this, std::placeholders::_1),
      std::bind(&AppProxy::isDomainNameValid, this, std::placeholders::_1),
    }));
  errors.append(getNumericConfigError(serverConfig, "serverPort",
                                      tr("Server Port"), 0, 65535));
  errors.append(getStringConfigError(serverConfig, "id", tr("ID")));
  errors.append(
    getNumericConfigError(serverConfig, "alterId", tr("Alter ID"), 0, 65535));
  errors.append(getStringConfigError(serverConfig, "security", tr("Security")));
  errors.append(
    getNumericConfigError(serverConfig, "mux", tr("MUX"), -1, 1024));
  errors.append(getStringConfigError(serverConfig, "network", tr("Network")));
  errors.append(getStringConfigError(serverConfig, "networkSecurity",
                                     tr("Network Security")));
  errors.append(getV2RayStreamSettingsErrors(
    serverConfig, serverConfig["network"].toString()));

  // Remove empty error messages generated by getNumericConfigError() and
  // getStringConfigError()
  errors.removeAll("");
  return errors;
}

QStringList AppProxy::getV2RayStreamSettingsErrors(
  const QJsonObject& serverConfig, const QString& network) {
  QStringList errors;
  if (network == "kcp") {
    errors.append(
      getNumericConfigError(serverConfig, "kcpMtu", tr("MTU"), 576, 1460));
    errors.append(
      getNumericConfigError(serverConfig, "kcpTti", tr("TTI"), 10, 100));
    errors.append(getNumericConfigError(serverConfig, "kcpUpLink",
                                        tr("Uplink Capacity"), 0, -127));
    errors.append(getNumericConfigError(serverConfig, "kcpDownLink",
                                        tr("Downlink Capacity"), 0, -127));
    errors.append(getNumericConfigError(serverConfig, "kcpReadBuffer",
                                        tr("Read Buffer Size"), 0, -127));
    errors.append(getNumericConfigError(serverConfig, "kcpWriteBuffer",
                                        tr("Write Buffer Size"), 0, -127));
    errors.append(
      getStringConfigError(serverConfig, "packetHeader", tr("Packet Header")));
  } else if (network == "ws" || network == "http") {
    errors.append(getStringConfigError(
      serverConfig, "networkHost", tr("Host"),
      {std::bind(&AppProxy::isDomainNameValid, this, std::placeholders::_1)}));
    errors.append(
      getStringConfigError(serverConfig, "networkPath", tr("Path")));
  } else if (network == "domainsocket") {
    errors.append(getStringConfigError(
      serverConfig, "domainSocketFilePath", tr("Socket File Path"),
      {std::bind(&AppProxy::isFileExists, this, std::placeholders::_1)}));
  } else if (network == "quic") {
    errors.append(
      getStringConfigError(serverConfig, "quicSecurity", tr("QUIC Security")));
    errors.append(
      getStringConfigError(serverConfig, "packetHeader", tr("Packet Header")));
    errors.append(
      getStringConfigError(serverConfig, "quicKey", tr("QUIC Key")));
  }
  return errors;
}

QString AppProxy::getStringConfigError(
  const QJsonObject& serverConfig,
  const QString& key,
  const QString& name,
  const QList<std::function<bool(const QString&)>>& checkpoints) {
  if (!serverConfig.contains(key) || serverConfig[key].toString().isEmpty()) {
    return QString(tr("Missing the value of '%1'.")).arg(name);
  }
  if (checkpoints.size() > 0) {
    bool isMatched = false;
    for (std::function<bool(const QString&)> ckpt : checkpoints) {
      if (ckpt(serverConfig[key].toString())) {
        isMatched = true;
      }
    }
    if (!isMatched) {
      return QString(tr("The value of '%1' seems invalid.")).arg(name);
    }
  }
  return "";
}

bool AppProxy::isIpAddrValid(const QString& ipAddr) {
  QRegularExpression ipAddrRegex(
    "^(([0-9]|[1-9][0-9]|1[0-9]{2}|2[0-4][0-9]|25[0-5])\\.){3}([0-9]|[1-9][0-9]"
    "|1[0-9]{2}|2[0-4][0-9]|25[0-5])$");
  return ipAddrRegex.match(ipAddr).hasMatch();
}

bool AppProxy::isDomainNameValid(const QString& domainName) {
  QRegularExpression domainNameRegex(
    "^(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\\.)+[a-z0-9][a-z0-9-]{0,61}[a-z0-"
    "9]$");
  return domainNameRegex.match(domainName).hasMatch();
}

bool AppProxy::isUrlValid(const QString& url) {
  QRegularExpression urlRegex(
    "^https?://(-\\.)?([^\\s/?\\.#-]+\\.?)+(/[^\\s]*)?$",
    QRegularExpression::CaseInsensitiveOption);
  return urlRegex.match(url).hasMatch();
}

bool AppProxy::isFileExists(const QString& filePath) {
  return QDir(filePath).exists();
}

QString AppProxy::getNumericConfigError(const QJsonObject& serverConfig,
                                        const QString& key,
                                        const QString& name,
                                        int lowerBound,
                                        int upperBound) {
  if (!serverConfig.contains(key) || serverConfig[key].toString().isEmpty()) {
    return QString(tr("Missing the value of '%1'.")).arg(name);
  } else {
    bool isConverted = false;
    int value        = serverConfig[key].toString().toInt(&isConverted);
    if (!isConverted) {
      return QString(tr("The value of '%1' seems invalid.")).arg(name);
    } else if (upperBound == -127 && value < lowerBound) {
      return QString(tr("The value of '%1' should above %2."))
        .arg(name, QString::number(lowerBound));
    } else if (value < lowerBound || value > upperBound) {
      return QString(tr("The value of '%1' should between %2 and %3."))
        .arg(name, QString::number(lowerBound), QString::number(upperBound));
    }
    return "";
  }
}

QJsonObject AppProxy::getPrettyV2RayConfig(const QJsonObject& serverConfig) {
  QJsonObject v2RayConfig{
    {"autoConnect", serverConfig["autoConnect"].toBool()},
    {"serverName", serverConfig["serverName"].toString()},
    {"subscription", serverConfig.contains("subscription")
                       ? serverConfig["subscription"].toString()
                       : ""},
    {"protocol", "vmess"},
    {"mux",
     QJsonObject{
       {"enabled", serverConfig["mux"].toString().toInt() != 1},
       {"concurrency", serverConfig["mux"].toString().toInt()},
     }},
    {"settings",
     QJsonObject{
       {"vnext",
        QJsonArray{QJsonObject{
          {"address", serverConfig["serverAddr"].toString()},
          {"port", serverConfig["serverPort"].toString().toInt()},
          {"users",
           QJsonArray{QJsonObject{
             {"id", serverConfig["id"].toString()},
             {"alterId", serverConfig["alterId"].toString().toInt()},
             {"security", serverConfig["security"].toString().toLower()},
           }}}}}}}},
    {"tag", "proxy-vmess"}};

  QJsonObject streamSettings = getV2RayStreamSettingsConfig(serverConfig);
  v2RayConfig.insert("streamSettings", streamSettings);
  return v2RayConfig;
}

QJsonObject AppProxy::getV2RayStreamSettingsConfig(
  const QJsonObject& serverConfig) {
  QString network = serverConfig["network"].toString();
  QJsonObject streamSettings{
    {"network", serverConfig["network"]},
    {"security", serverConfig["networkSecurity"].toString().toLower()},
    {"tlsSettings",
     QJsonObject{{"allowInsecure", serverConfig["allowInsecure"].toBool()}}},
  };

  if (network == "tcp") {
    QString tcpHeaderType = serverConfig["tcpHeaderType"].toString().toLower();
    QJsonObject tcpSettings{{"type", tcpHeaderType}};
    if (tcpHeaderType == "http") {
      tcpSettings.insert(
        "request",
        QJsonObject{
          {"version", "1.1"},
          {"method", "GET"},
          {"path", QJsonArray{"/"}},
          {"headers",
           QJsonObject{
             {"host",
              QJsonArray{"www.baidu.com", "www.bing.com", "www.163.com",
                         "www.netease.com", "www.qq.com", "www.tencent.com",
                         "www.taobao.com", "www.tmall.com",
                         "www.alibaba-inc.com", "www.aliyun.com",
                         "www.sensetime.com", "www.megvii.com"}},
             {"User-Agent", getRandomUserAgents(24)},
             {"Accept-Encoding", QJsonArray{"gzip, deflate"}},
             {"Connection", QJsonArray{"keep-alive"}},
             {"Pragma", "no-cache"},
           }},
        });
      tcpSettings.insert(
        "response",
        QJsonObject{
          {"version", "1.1"},
          {"status", "200"},
          {"reason", "OK"},
          {"headers",
           QJsonObject{{"Content-Type", QJsonArray{"text/html;charset=utf-8"}},
                       {"Transfer-Encoding", QJsonArray{"chunked"}},
                       {"Connection", QJsonArray{"keep-alive"}},
                       {"Pragma", "no-cache"}}}});
    }
    streamSettings.insert("tcpSettings", tcpSettings);
  } else if (network == "kcp") {
    streamSettings.insert(
      "kcpSettings",
      QJsonObject{
        {"mtu", serverConfig["kcpMtu"].toString().toInt()},
        {"tti", serverConfig["kcpTti"].toString().toInt()},
        {"uplinkCapacity", serverConfig["kcpUpLink"].toString().toInt()},
        {"downlinkCapacity", serverConfig["kcpDownLink"].toString().toInt()},
        {"congestion", serverConfig["kcpCongestion"].toBool()},
        {"readBufferSize", serverConfig["kcpReadBuffer"].toString().toInt()},
        {"writeBufferSize", serverConfig["kcpWriteBuffer"].toString().toInt()},
        {"header",
         QJsonObject{
           {"type", serverConfig["packetHeader"].toString().toLower()}}}});
  } else if (network == "ws") {
    streamSettings.insert(
      "wsSettings",
      QJsonObject{
        {"path", serverConfig["networkPath"].toString()},
        {"headers", QJsonObject{{"host", serverConfig["networkHost"]}}}});
  } else if (network == "http") {
    streamSettings.insert(
      "httpSettings",
      QJsonObject{
        {"host", QJsonArray{serverConfig["networkHost"].toString()}},
        {"path", QJsonArray{serverConfig["networkPath"].toString()}}});
  } else if (network == "domainsocket") {
    streamSettings.insert(
      "dsSettings",
      QJsonObject{{"path", serverConfig["domainSocketFilePath"].toString()}});
  } else if (network == "quic") {
    streamSettings.insert(
      "quicSettings",
      QJsonObject{
        {"security", serverConfig["quicSecurity"].toString().toLower()},
        {"key", serverConfig["quicKey"].toString()},
        {"header",
         QJsonObject{
           {"type", serverConfig["packetHeader"].toString().toLower()}}}});
  }
  return streamSettings;
}

QJsonArray AppProxy::getRandomUserAgents(int n) {
  QStringList OPERATING_SYSTEMS{"Macintosh; Intel Mac OS X 10_15",
                                "X11; Linux x86_64",
                                "Windows NT 10.0; Win64; x64"};
  QJsonArray userAgents;
  for (int i = 0; i < n; ++i) {
    int osIndex            = std::rand() % 3;
    int chromeMajorVersion = std::rand() % 30 + 50;
    int chromeBuildVersion = std::rand() % 4000 + 1000;
    int chromePatchVersion = std::rand() % 100;
    userAgents.append(QString("Mozilla/5.0 (%1) AppleWebKit/537.36 (KHTML, "
                              "like Gecko) Chrome/%2.0.%3.%4 Safari/537.36")
                        .arg(OPERATING_SYSTEMS[osIndex],
                             QString::number(chromeMajorVersion),
                             QString::number(chromeBuildVersion),
                             QString::number(chromePatchVersion)));
  }
  return userAgents;
}

void AppProxy::addShadowsocksServer(QString configString) {
  QJsonDocument configDoc  = QJsonDocument::fromJson(configString.toUtf8());
  QJsonObject serverConfig = configDoc.object();
  // Check server config before saving
  QStringList serverConfigErrors =
    getShadowsocksServerConfigErrors(serverConfig);
  if (serverConfigErrors.size() > 0) {
    emit serverConfigError(serverConfigErrors.join('\n'));
    return;
  }
  // Save server config
  configurator.addServer(getPrettyShadowsocksConfig(serverConfig));
  emit serversChanged();
  qInfo() << QString("Add new Shadowsocks server [Name=%1, Addr=%2].")
               .arg(serverConfig["serverName"].toString(),
                    serverConfig["serverAddr"].toString());
}

QStringList AppProxy::getShadowsocksServerConfigErrors(
  const QJsonObject& serverConfig) {
  QStringList errors;
  errors.append(
    getStringConfigError(serverConfig, "serverName", tr("Server Name")));
  errors.append(getStringConfigError(
    serverConfig, "serverAddr", tr("Server Address"),
    {
      std::bind(&AppProxy::isIpAddrValid, this, std::placeholders::_1),
      std::bind(&AppProxy::isDomainNameValid, this, std::placeholders::_1),
    }));
  errors.append(getNumericConfigError(serverConfig, "serverPort",
                                      tr("Server Port"), 0, 65535));
  errors.append(
    getStringConfigError(serverConfig, "encryption", tr("Security")));
  errors.append(getStringConfigError(serverConfig, "password", tr("Password")));

  // Remove empty error messages generated by getNumericConfigError() and
  // getStringConfigError()
  errors.removeAll("");
  return errors;
}

QJsonObject AppProxy::getPrettyShadowsocksConfig(
  const QJsonObject& serverConfig) {
  return QJsonObject{
    {"autoConnect", serverConfig["autoConnect"].toBool()},
    {"serverName", serverConfig["serverName"].toString()},
    {"subscription", serverConfig.contains("subscription")
                       ? serverConfig["subscription"].toString()
                       : ""},
    {"protocol", "shadowsocks"},
    {"settings",
     QJsonObject{{"servers",
                  QJsonArray{QJsonObject{
                    {"address", serverConfig["serverAddr"].toString()},
                    {"port", serverConfig["serverPort"].toString().toInt()},
                    {"method", serverConfig["encryption"].toString().toLower()},
                    {"password", serverConfig["password"].toString()}}}}}},
    {"streamSettings", QJsonObject{{"network", "tcp"}}},
    {"tag", "proxy-shadowsocks"}};
}

void AppProxy::addSubscriptionUrl(QString subsriptionUrl) {
  QString error = getStringConfigError(
    {{"subsriptionUrl", subsriptionUrl}}, "subsriptionUrl",
    tr("Subscription URL"),
    {std::bind(&AppProxy::isUrlValid, this, std::placeholders::_1)});
  if (!error.isEmpty()) {
    emit serverConfigError(error);
    return;
  }
  updateSubscriptionServers(subsriptionUrl);
}

void AppProxy::updateSubscriptionServers(QString subsriptionUrl) {
  QStringList subscriptionUrls;
  if (subsriptionUrl.isEmpty()) {
    subscriptionUrls = configurator.getSubscriptionUrls();
  } else {
    subscriptionUrls.append(subsriptionUrl);
  }
  for (QString su : subscriptionUrls) {
    emit getSubscriptionServersStarted(su, getQProxy());
  }
}

void AppProxy::addSubscriptionServers(QString subsriptionServers,
                                      QString subsriptionUrl) {
  if (!subsriptionServers.size()) {
    emit serverConfigError("Failed to get subscription servers from URL.");
    return;
  }
  // Remove servers from the subscription if exists
  QMap<QString, QJsonObject> removedServers =
    configurator.removeSubscriptionServers(subsriptionUrl);
  // Add new servers
  QStringList servers = subsriptionServers.split('\n');
  QJsonObject serverConfig;
  for (QString server : servers) {
    if (server.startsWith("ss://")) {
      serverConfig = getShadowsocksServerFromUrl(server, subsriptionUrl);
      if (serverConfig.contains("obfs")) {
        qWarning() << "Ignore Shadowsocks server with obfs: " << serverConfig;
        continue;
      }
      serverConfig = getPrettyShadowsocksConfig(serverConfig);
    } else if (server.startsWith("vmess://")) {
      serverConfig =
        getPrettyV2RayConfig(getV2RayServerFromUrl(server, subsriptionUrl));
    } else {
      qWarning() << QString("Ignore subscription server: %1").arg(server);
      continue;
    }
    // Recover auto connect option for the server
    QString serverName = serverConfig.contains("serverName")
                           ? serverConfig["serverName"].toString()
                           : "";
    serverConfig["autoConnect"] =
      removedServers.contains(serverName)
        ? removedServers[serverName]["autoConnect"].toBool()
        : false;
    configurator.addServer(serverConfig);
    qInfo() << QString("Add a new server[Name=%1] from URI: %2")
                 .arg(serverName, server);
  }
  emit serversChanged();
}

QJsonObject AppProxy::getV2RayServerFromUrl(const QString& server,
                                            const QString& subscriptionUrl) {
  // Ref:
  // https://github.com/2dust/v2rayN/wiki/%E5%88%86%E4%BA%AB%E9%93%BE%E6%8E%A5%E6%A0%BC%E5%BC%8F%E8%AF%B4%E6%98%8E(ver-2)
  const QMap<QString, QString> NETWORK_MAPPER = {
    {"tcp", "tcp"}, {"kcp", "kcp"},   {"ws", "ws"},
    {"h2", "http"}, {"quic", "quic"},
  };
  QJsonObject rawServerConfig =
    QJsonDocument::fromJson(QByteArray::fromBase64(server.mid(8).toUtf8()))
      .object();
  QString network =
    rawServerConfig.contains("net") ? rawServerConfig["net"].toString() : "tcp";
  QString serverAddr =
    rawServerConfig.contains("add") ? rawServerConfig["add"].toString() : "";
  QJsonObject serverConfig{
    {"autoConnect", false},
    {"serverName", rawServerConfig.contains("ps")
                     ? rawServerConfig["ps"].toString()
                     : serverAddr},
    {"serverAddr", serverAddr},
    {"serverPort",
     rawServerConfig.contains("port") ? rawServerConfig["port"].toInt() : 0},
    {"subscription", subscriptionUrl},
    {"id",
     rawServerConfig.contains("id") ? rawServerConfig["id"].toString() : ""},
    {"alterId", rawServerConfig.contains("aid")
                  ? rawServerConfig["aid"].toString().toInt()
                  : 0},
    {"mux", -1},
    {"security", "auto"},
    {"network",
     NETWORK_MAPPER.contains(network) ? NETWORK_MAPPER[network] : "tcp"},
    {"networkHost", rawServerConfig.contains("host")
                      ? rawServerConfig["host"].toString()
                      : ""},
    {"networkPath", rawServerConfig.contains("path")
                      ? rawServerConfig["path"].toString()
                      : ""},
    {"tcpHeaderType", rawServerConfig.contains("type")
                        ? rawServerConfig["type"].toString()
                        : ""},
    {"networkSecurity", rawServerConfig.contains("tls") ? "tls" : "none"}};
  return serverConfig;
}

QJsonObject AppProxy::getShadowsocksServerFromUrl(
  QString serverUrl, const QString& subscriptionUrl) {
  serverUrl             = serverUrl.mid(5);
  int atIndex           = serverUrl.indexOf('@');
  int colonIndex        = serverUrl.indexOf(':');
  int splashIndex       = serverUrl.indexOf('/');
  int sharpIndex        = serverUrl.indexOf('#');
  int questionMarkIndex = serverUrl.indexOf('?');

  QString confidential =
    QByteArray::fromBase64(serverUrl.left(atIndex).toUtf8());
  QString serverAddr = serverUrl.mid(atIndex + 1, colonIndex - atIndex - 1);
  int serverPort =
    serverUrl.mid(colonIndex + 1, splashIndex - colonIndex - 1).toInt();
  QString options =
    serverUrl.mid(questionMarkIndex + 1, sharpIndex - questionMarkIndex - 1);
  QString serverName =
    QUrl::fromPercentEncoding(serverUrl.mid(sharpIndex + 1).toUtf8());

  colonIndex         = confidential.indexOf(':');
  QString encryption = confidential.left(colonIndex);
  QString password   = confidential.mid(colonIndex + 1);

  QJsonObject serverConfig{{"serverName", serverName},
                           {"autoConnect", false},
                           {"subscription", subscriptionUrl},
                           {"serverAddr", serverAddr},
                           {"serverPort", serverPort},
                           {"encryption", encryption},
                           {"password", password}};

  QJsonObject obfsOptions;
  for (QPair<QString, QString> p : QUrlQuery(options).queryItems()) {
    if (p.first == "plugin") {
      QStringList obfs =
        QUrl::fromPercentEncoding(p.second.toUtf8()).split(';');
      for (QString o : obfs) {
        QStringList t = o.split('=');
        if (t.size() == 2) {
          obfsOptions[t[0]] = t[1];
        }
      }
    }
  }
  if (obfsOptions.size()) {
    serverConfig["obfs"] = obfsOptions;
  }
  return serverConfig;
}

void AppProxy::addServerConfigFile(QString configFilePath) {}

void AppProxy::editServer(QString serverName,
                          QString protocol,
                          QString configString) {
  QJsonDocument configDoc  = QJsonDocument::fromJson(configString.toUtf8());
  QJsonObject serverConfig = configDoc.object();
  QStringList serverConfigErrors =
    getServerConfigErrors(serverConfig, protocol);
  if (serverConfigErrors.size() > 0) {
    emit serverConfigError(serverConfigErrors.join('\n'));
    return;
  }
  serverConfig = getPrettyServerConfig(serverConfig, protocol);

  if (configurator.editServer(serverName, serverConfig)) {
    QString newServerName = serverConfig["serverName"].toString();
    // Update the information of server connectivity
    QStringList connectedServerNames = configurator.getConnectedServerNames();
    serverConfig["connected"] = connectedServerNames.contains(newServerName);
    // Update the server latency even if the server name is changed
    if (serverLatency.contains(serverName)) {
      serverConfig["latency"] = serverLatency[serverName].toInt();
      if (newServerName != serverName) {
        serverLatency.insert(newServerName, serverLatency[serverName]);
        serverLatency.remove(serverName);
      }
    }
    emit serverChanged(serverName, QJsonDocument(serverConfig).toJson());
    // Restart V2Ray Core
    v2ray.restart();
  }
}

QStringList AppProxy::getServerConfigErrors(const QJsonObject& serverConfig,
                                            QString protocol) {
  if (protocol == "vmess") {
    return getV2RayServerConfigErrors(serverConfig);
  } else if (protocol == "shadowsocks") {
    return getShadowsocksServerConfigErrors(serverConfig);
  } else {
    return {QString("Unknown Protocol: %1").arg(protocol)};
  }
}

QJsonObject AppProxy::getPrettyServerConfig(const QJsonObject& serverConfig,
                                            QString protocol) {
  if (protocol == "vmess") {
    return getPrettyV2RayConfig(serverConfig);
  } else if (protocol == "shadowsocks") {
    return getPrettyShadowsocksConfig(serverConfig);
  }
  return QJsonObject{};
}

void AppProxy::removeServer(QString serverName) {
  configurator.removeServer(serverName);
  qInfo() << QString("Server [Name=%1] have been removed.").arg(serverName);
  emit serverRemoved(serverName);
  // Restart V2Ray Core
  v2ray.restart();
}

void AppProxy::removeSubscriptionServers(QString subscriptionUrl) {
  configurator.removeSubscriptionServers(subscriptionUrl);
  emit serversChanged();
}

void AppProxy::scanQrCodeScreen() {
  QStringList servers;
  QList<QScreen*> screens = QGuiApplication::screens();

  for (int i = 0; i < screens.size(); ++i) {
    QRect r = screens.at(i)->geometry();
    QPixmap screenshot =
      screens.at(i)->grabWindow(0, r.x(), r.y(), r.width(), r.height());
    QString serverUrl = QrCodeHelper::decode(
      screenshot.toImage().convertToFormat(QImage::Format_Grayscale8));
    if (serverUrl.size()) {
      servers.append(serverUrl);
    }
  }
  qInfo() << QString("Add %1 servers from QR code.")
               .arg(QString::number(servers.size()));
  addSubscriptionServers(servers.join('\n'));
}

void AppProxy::copyToClipboard(QString text) {
  QClipboard* clipboard = QGuiApplication::clipboard();
  clipboard->setText(text, QClipboard::Clipboard);
}
