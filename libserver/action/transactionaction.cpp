/*
 * transactionaction.cpp
 * Copyright 2017 - ~, Apin <apin.klas@gmail.com>
 *
 * This file is part of Sultan.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include "transactionaction.h"
#include "db.h"
#include "dbresult.h"
#include "global_constant.h"
#include "message.h"
#include "preference.h"
#include "queryhelper.h"
#include <QBuffer>
#include <QDebug>
#include <QStringBuilder>

#include "header/xlsxdocument.h"

using namespace LibServer;
using namespace LibG;
using namespace LibDB;

TransactionAction::TransactionAction() : ServerAction("transactions", "id") {
    mFlag = USE_TRANSACTION;
    mFunctionMap.insert(MSG_COMMAND::SUMMARY,
                        std::bind(&TransactionAction::summaryCashier, this, std::placeholders::_1));
    mFunctionMap.insert(MSG_COMMAND::SUMMARY_TRANSACTION,
                        std::bind(&TransactionAction::summaryTransaction, this, std::placeholders::_1));
    mFunctionMap.insert(MSG_COMMAND::SUMMARY_MONEY,
                        std::bind(&TransactionAction::summaryMoney, this, std::placeholders::_1));
    mFunctionMap.insert(MSG_COMMAND::EXPORT, std::bind(&TransactionAction::exportData, this, std::placeholders::_1));
}

LibG::Message TransactionAction::summaryTransaction(LibG::Message *msg) {
    Message message(msg);
    mDb->table(mTableName);
    mDb = QueryHelper::filter(mDb, msg->data(), fieldMap());
    DbResult res = mDb->clone()->select("sum(transaction_total) as total")->exec();
    message.addData("total", res.first()["total"]);
    res = mDb->clone()->select("sum(transaction_total) as income")->where("type = ", TRANSACTION_TYPE::INCOME)->exec();
    message.addData("income", res.first()["income"]);
    res =
        mDb->clone()->select("sum(transaction_total) as expense")->where("type = ", TRANSACTION_TYPE::EXPENSE)->exec();
    message.addData("expense", res.first()["expense"].toDouble());
    res = mDb->clone()
              ->select("sum(transaction_total) as transonly")
              ->where("type = ", TRANSACTION_TYPE::INCOME)
              ->where("link_type = ", TRANSACTION_LINK_TYPE::TRANSACTION)
              ->exec();
    message.addData("transonly", res.first()["transonly"]);
    // Margin
    msg->keepFilter(QStringList() << "0$date"
                                  << "1$date");
    mDb = QueryHelper::filter(mDb->reset(), msg->data(), QMap<QString, QString>{{"date", "DATE(created_at)"}});
    res = mDb->where(QString("(flag & %1) == 0").arg(ITEM_FLAG::ITEM_LINK))
              ->select("sum(final - buy_price) as margin")
              ->get("solditems");
    message.addData("margin", res.first()["margin"]);
    return message;
}

LibG::Message TransactionAction::summaryMoney(LibG::Message *msg) {
    Message message(msg);
    mDb->table(mTableName);
    mDb = QueryHelper::filter(mDb, msg->data(), fieldMap());
    DbResult res = mDb->clone()
                       ->select("sum(money_total) as total")
                       ->join("left join users on users.id = transactions.user_id")
                       ->exec();
    message.addData("total", res.first()["total"].toDouble());
    return message;
}

Message TransactionAction::exportData(Message *msg) {
    LibG::Message message(msg);

    QXlsx::Document xlsx;
    int row = 1;
    int col = 1;
    QStringList headers;
    headers << "date"
            << "number"
            << "type"
            << "detail"
            << "total";
    for (auto header : headers) {
        xlsx.write(row, col++, header);
    }
    row++;

    mDb->table(mTableName);
    selectAndJoin();
    mDb = QueryHelper::filter(mDb, msg->data(), fieldMap());
    mDb = QueryHelper::sort(mDb, msg->data());
    const int limit = 500;
    int start = 0;
    while (true) {
        DbResult res = mDb->clone()->start(start)->limit(limit)->exec();
        if (res.isEmpty())
            break;
        for (int i = 0; i < res.size(); i++) {
            const QVariantMap &d = res.data(i);
            xlsx.write(row, 1, d["date"]);
            xlsx.write(row, 2, d["number"]);
            xlsx.write(row, 3, d["type"].toInt() == TRANSACTION_TYPE::INCOME ? "income" : "expense");
            xlsx.write(row, 4, d["detail"]);
            xlsx.write(row, 5, d["transaction_total"]);
            row++;
        }
        start += limit;
    }

    QBuffer arr;
    xlsx.saveAs(&arr);
    message.addData("data", QString(arr.data().toBase64()));
    return message;
}

Message TransactionAction::summaryCashier(Message *msg) {
    LibG::Message message(msg);
    int machineid = msg->data("machine_id").toInt();
    // get the users
    mDb->where("transactions.date >= ", msg->data("start"))
        ->where("transactions.date < ", msg->data("end"))
        ->where("transactions.machine_id = ", machineid);
    DbResult res = mDb->clone()
                       ->select("DISTINCT(user_id), users.name as name")
                       ->join("LEFT JOIN users ON users.id = transactions.user_id")
                       ->get("transactions");
    if (res.isEmpty())
        return message;
    QVariantList l;
    for (int i = 0; i < res.size(); i++) {
        QVariantMap d{{"user", res.data(i)["name"]}};
        DbResult r = mDb->clone()
                         ->select("link_type, SUM(money_total) as total")
                         ->where("transactions.user_id =", res.data(i)["user_id"])
                         ->group("transactions.type")
                         ->get("transactions");
        d["type"] = r.data();
        r = mDb->clone()
                ->select("transactions.bank_id, banks.name as bank, SUM(transactions.money_total) as total")
                ->join("LEFT JOIN banks ON banks.id = transactions.bank_id")
                ->where("transactions.user_id =", res.data(i)["user_id"])
                ->group("transactions.bank_id")
                ->get("transactions");
        d["bank"] = r.data();
        l.append(d);
    }
    {
        QVariantMap d{{"user", QObject::tr("All")}};
        DbResult r = mDb->clone()
                         ->select("link_type, SUM(money_total) as total")
                         ->group("transactions.type")
                         ->get("transactions");
        d["type"] = r.data();
        r = mDb->clone()
                ->select("transactions.bank_id, banks.name as bank, SUM(transactions.money_total) as total")
                ->join("LEFT JOIN banks ON banks.id = transactions.bank_id")
                ->group("transactions.bank_id")
                ->get("transactions");
        d["bank"] = r.data();
        l.append(d);
    }
    message.addData("data", l);
    return message;
}

QMap<QString, QString> TransactionAction::fieldMap() const {
    QMap<QString, QString> map;
    map.insert("user", "users.username");
    map.insert("machine", "machines.name");
    map.insert("bank", "transactions.bank_id");
    return map;
}

void TransactionAction::selectAndJoin() {
    mDb->table(mTableName)
        ->select("transactions.*, users.username as user, banks.name as bank, machines.name as machine")
        ->join("LEFT JOIN users ON transactions.user_id = users.id")
        ->join("LEFT JOIN banks ON transactions.bank_id = banks.id")
        ->join("LEFT JOIN machines ON transactions.machine_id = machines.id");
}
