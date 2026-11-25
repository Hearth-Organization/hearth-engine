/*
 * Hearth Engine: talking to the Hearth Engine agent from the editor (localhost API, token from
 * ~/.config/hearth-engine/agent.json). Header only. Built on O3DE (Apache-2.0 OR MIT).
 */
#pragma once

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <functional>

class HearthAgentClient
{
public:
    //! answer: the agent's JSON; reached: false when the agent couldn't be reached at all.
    using Callback = std::function<void(const QJsonObject& answer, bool reached)>;

    explicit HearthAgentClient(QObject* owner)
        : m_owner(owner)
        , m_network(new QNetworkAccessManager(owner))
    {
    }

    void Post(const QString& op, const QJsonObject& args, Callback done, int timeoutMs = 20000)
    {
        if (m_token.isEmpty())
        {
            ReadAgentFile();
        }
        if (m_token.isEmpty())
        {
            if (done)
            {
                done(QJsonObject{ { QStringLiteral("ok"), false }, { QStringLiteral("error"), QStringLiteral("the Hearth Engine agent isn't running") } }, false);
            }
            return;
        }
        QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/api/%2").arg(m_port).arg(op)));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("X-Hearth-Token", m_token.toUtf8());
        QNetworkReply* reply = m_network->post(request, QJsonDocument(args).toJson(QJsonDocument::Compact));
        QPointer<QNetworkReply> guard(reply);
        QTimer::singleShot(timeoutMs, reply, [guard]()
        {
            if (guard && guard->isRunning())
            {
                guard->abort();
            }
        });
        QObject::connect(reply, &QNetworkReply::finished, m_owner, [this, reply, done]()
        {
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 0)
            {
                m_token.clear();
                if (done)
                {
                    done(QJsonObject{ { QStringLiteral("ok"), false }, { QStringLiteral("error"), reply->errorString() } }, false);
                }
                return;
            }
            if (status == 403)
            {
                m_token.clear();
            }
            if (done)
            {
                done(QJsonDocument::fromJson(reply->readAll()).object(), true);
            }
        });
    }

private:
    void ReadAgentFile()
    {
        const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        QFile file(QDir(config).filePath(QStringLiteral("hearth-engine/agent.json")));
        if (file.open(QIODevice::ReadOnly))
        {
            const QJsonObject agent = QJsonDocument::fromJson(file.readAll()).object();
            m_token = agent.value(QStringLiteral("token")).toString();
            m_port = agent.value(QStringLiteral("port")).toInt(47830);
        }
    }

    QObject* m_owner = nullptr;
    QNetworkAccessManager* m_network = nullptr;
    QString m_token;
    int m_port = 47830;
};
