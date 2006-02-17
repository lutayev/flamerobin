/*
  The contents of this file are subject to the Initial Developer's Public
  License Version 1.0 (the "License"); you may not use this file except in
  compliance with the License. You may obtain a copy of the License here:
  http://www.flamerobin.org/license.html.

  Software distributed under the License is distributed on an "AS IS"
  basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
  License for the specific language governing rights and limitations under
  the License.

  The Original Code is FlameRobin (TM).

  The Initial Developer of the Original Code is Milan Babuskov.

  Portions created by the original developer
  are Copyright (C) 2004 Milan Babuskov.

  All Rights Reserved.

  $Id$

  Contributor(s): Nando Dessena
*/

// For compilers that support precompilation, includes "wx/wx.h".
#include "wx/wxprec.h"

// for all others, include the necessary headers (this file is usually all you
// need because it includes almost all "standard" wxWindows headers
#ifndef WX_PRECOMP
    #include "wx/wx.h"
#endif

#ifdef __BORLANDC__
    #pragma hdrstop
#endif

#include <ibpp.h>

#include "core/Visitor.h"
#include "dberror.h"
#include "frutils.h"
#include "metadata/collection.h"
#include "metadata/CreateDDLVisitor.h"
#include "metadata/database.h"
#include "metadata/MetadataItemVisitor.h"
#include "metadata/view.h"
#include "ugly.h"
//-----------------------------------------------------------------------------
View::View()
{
    typeM = ntView;
}
//-----------------------------------------------------------------------------
//! returns false if an error occurs
bool View::getSource(wxString& source)
{
    source = wxT("");
    Database *d = getDatabase();
    if (!d)
    {
        lastError().setMessage(wxT("Database not set."));
        return false;
    }

    IBPP::Database& db = d->getIBPPDatabase();

    try
    {
        IBPP::Transaction tr1 = IBPP::TransactionFactory(db, IBPP::amRead);
        tr1->Start();
        IBPP::Statement st1 = IBPP::StatementFactory(db, tr1);
        st1->Prepare("select rdb$view_source from rdb$relations where rdb$relation_name = ?");
        st1->Set(1, wx2std(getName_()));
        st1->Execute();
        st1->Fetch();
        readBlob(st1, 1, source);
        tr1->Commit();
        return true;
    }
    catch (IBPP::Exception &e)
    {
        lastError().setMessage(std2wx(e.ErrorMessage()));
    }
    catch (...)
    {
        lastError().setMessage(_("System error."));
    }

    return false;
}
//-----------------------------------------------------------------------------
wxString View::getCreateSql()
{
    wxString src;
    if (!checkAndLoadColumns())
        return lastError().getMessage();
    if (!getSource(src))
        return lastError().getMessage();

    wxString sql;
    sql += wxT("CREATE VIEW ") + getQuotedName() + wxT(" (");

    bool first = true;
    for (MetadataCollection <Column>::const_iterator it = columnsM.begin(); it != columnsM.end(); ++it)
    {
        if (first)
            first = false;
        else
            sql += wxT(", ");
        sql += (*it).getQuotedName();
    }
    sql += wxT(")\nAS ");
    sql += src;
    sql += wxT(";\n");
    return sql;
}
//-----------------------------------------------------------------------------
wxString View::getAlterSql()
{
    wxString sql = wxT("DROP VIEW ") + getQuotedName() + wxT(";\n");
    sql += getCreateSql();

    // Restore user privileges
    const std::vector<Privilege>* priv = getPrivileges();
    if (priv)
    {
        for (std::vector<Privilege>::const_iterator ci = priv->begin();
            ci != priv->end(); ++ci)
        {
            sql += (*ci).getSql() + wxT(";\n");
        }
    }
    return sql;
}
//-----------------------------------------------------------------------------
void View::getDependentChecks(std::vector<CheckConstraint>& checks)
{
    Database *d = getDatabase();
    if (!d)
        return;
    IBPP::Database& db = d->getIBPPDatabase();
    try
    {
        IBPP::Transaction tr1 = IBPP::TransactionFactory(db, IBPP::amRead);
        tr1->Start();
        IBPP::Statement st1 = IBPP::StatementFactory(db, tr1);
        st1->Prepare(
            "select c.rdb$constraint_name, t.rdb$relation_name, "
            "   t.rdb$trigger_source "
            "from rdb$check_constraints c "
            "join rdb$triggers t on c.rdb$trigger_name = t.rdb$trigger_name "
            "join rdb$dependencies d on "
            "   t.rdb$trigger_name = d.rdb$dependent_name "
            "where d.rdb$depended_on_name = ? "
            "and d.rdb$dependent_type = 2 and d.rdb$depended_on_type = 0 "
            "and t.rdb$trigger_type = 1 and d.rdb$field_name is null "
        );

        st1->Set(1, wx2std(getName_()));
        st1->Execute();
        while (st1->Fetch())
        {
            wxString source;
            std::string table, cname;
            st1->Get(1, cname);
            st1->Get(2, table);
            readBlob(st1, 3, source);
            wxString consname = std2wx(cname).Strip();
            Table *tab = dynamic_cast<Table *>(d->findByNameAndType(ntTable,
                std2wx(table).Strip()));
            if (!tab)
                continue;

            // check if it exists
            std::vector<CheckConstraint>::iterator it;
            for (it = checks.begin(); it != checks.end(); ++it)
                if ((*it).getTable() == tab && (*it).getName_() == consname)
                    break;

            if (it != checks.end())     // already there
                continue;

            CheckConstraint c;
            c.setParent(tab);
            c.setName_(consname);
            c.sourceM = source;
            checks.push_back(c);
        }
        tr1->Commit();
    }
    catch (IBPP::Exception &e)
    {
        lastError().setMessage(std2wx(e.ErrorMessage()));
    }
    catch (...)
    {
        lastError().setMessage(_("System error."));
    }
}
//-----------------------------------------------------------------------------
// STEPS:
// * drop dependent check constraints (checks reference this view)
//      these checks can include other objects that might be dropped here
//      put CREATE of all checks at the end
// * alter all dep. stored procedures (empty)
// * drop dep. views (BUILD DEPENDENCY TREE) + TODO: computed table columns
// * drop this view
// * create this view
// * create dep. views + TODO: computed table columns
// * alter back SPs
// * recreate triggers
// * create checks
// * grant back privileges on all! dropped views
wxString View::getRebuildSql()
{
    // 0. prepare stuff
    std::vector<Procedure *> procedures;
    std::vector<CheckConstraint> checks;
    wxString privileges, triggers, dropViews, createViews;

    // 1. build view list (dependency tree) - ordered by DROP
    std::vector<View *> viewList;
    getDependentViews(viewList);

    // 2. walk the tree and add stuff
    for (std::vector<View *>::iterator vi = viewList.begin();
        vi != viewList.end(); ++vi)
    {
        dropViews += wxT("DROP VIEW ") + (*vi)->getQuotedName() + wxT(";\n");
        createViews = (*vi)->getCreateSql() + wxT("\n") + createViews;

        std::vector<Dependency> list;               // procedures
        if ((*vi)->getDependencies(list, false))
        {
            for (std::vector<Dependency>::iterator it = list.begin();
                it != list.end(); ++it)
            {
                Procedure *p = dynamic_cast<Procedure *>(
                    (*it).getDependentObject());
                if (p && procedures.end() == std::find(procedures.begin(),
                    procedures.end(), p))
                {
                    procedures.push_back(p);
                }
            }
        }
        (*vi)->getDependentChecks(checks);

        std::vector<Trigger *> trigs;
        (*vi)->getTriggers(trigs, Trigger::afterTrigger);
        (*vi)->getTriggers(trigs, Trigger::beforeTrigger);
        for (std::vector<Trigger *>::iterator it = trigs.begin();
            it != trigs.end(); ++it)
        {
            CreateDDLVisitor cdv;
            (*it)->acceptVisitor(&cdv);
            triggers += cdv.getSql() + wxT("\n");
        }

        const std::vector<Privilege>* priv = (*vi)->getPrivileges();
        if (priv)
        {
            for (std::vector<Privilege>::const_iterator ci = priv->begin();
                ci != priv->end(); ++ci)
            {
                privileges += (*ci).getSql() + wxT(";\n");
            }
        }
    }

    wxString createChecks, dropChecks;
    for (std::vector<CheckConstraint>::iterator it = checks.begin();
        it != checks.end(); ++it)
    {
        wxString cname = wxT("CONSTRAINT ") + (*it).getQuotedName();
        createChecks += wxT("ALTER TABLE ") +
            (*it).getTable()->getQuotedName();
        dropChecks = createChecks;
        dropChecks += wxT(" DROP ") + cname + wxT(";\n");
        createChecks += wxT(" ADD ");
        if (!(*it).isSystem())
            createChecks += cname;
        createChecks += wxT("\n  ") + (*it).sourceM + wxT(";\n");
    }

    wxString sql(wxT("SET AUTODDL ON;\n\n"));
    sql += dropChecks;
    for (std::vector<Procedure *>::iterator it = procedures.begin();
        it != procedures.end(); ++it)
    {
        sql += wxT("\n/* ------------------------------------------ */\n\n")
            + (*it)->getAlterSql(false);
    }
    sql += dropViews;
    sql += wxT("\n/**************** DROPPING COMPLETE ***************/\n\n");
    sql += createViews;
    for (std::vector<Procedure *>::iterator it = procedures.begin();
        it != procedures.end(); ++it)
    {
        sql += wxT("\n/* ------------------------------------------ */\n\n")
            + (*it)->getAlterSql(true);
    }
    sql += triggers;
    sql += createChecks;
    sql += privileges;

    // TODO: restore view descriptions

    return sql;
}
//-----------------------------------------------------------------------------
void View::getDependentViews(std::vector<View *>& views)
{
    std::vector<Dependency> list;
    if (getDependencies(list, false))
    {
        for (std::vector<Dependency>::iterator it = list.begin();
            it != list.end(); ++it)
        {
            View *v = dynamic_cast<View *>((*it).getDependentObject());
            if (v && views.end() == std::find(views.begin(), views.end(), v))
                v->getDependentViews(views);
        }
    }
    views.push_back(this);
}
//-----------------------------------------------------------------------------
wxString View::getCreateSqlTemplate() const
{
    wxString sql(
        wxT("CREATE VIEW name ( view_column, ...)\n")
        wxT("AS\n")
        wxT("/* write select statement here */\n")
        wxT("WITH CHECK OPTION;\n"));
    return sql;
}
//-----------------------------------------------------------------------------
const wxString View::getTypeName() const
{
    return wxT("VIEW");
}
//-----------------------------------------------------------------------------
void View::acceptVisitor(MetadataItemVisitor* visitor)
{
    visitor->visit(*this);
}
//-----------------------------------------------------------------------------
