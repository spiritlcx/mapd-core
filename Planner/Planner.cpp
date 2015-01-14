/**
 * @file		Planner.cpp
 * @author	Wei Hong <wei@map-d.com>
 * @brief		Functions for query plan nodes
 * 
 * Copyright (c) 2014 MapD Technologies, Inc.  All rights reserved.
 **/

#include <cassert>
#include <iostream>
#include <stdexcept>
#include "../Analyzer/Analyzer.h"
#include "Planner.h"

namespace Planner {
	Plan::~Plan()
	{
		for (auto p : targetlist)
			delete p;
		for (auto p : quals)
			delete p;
		if (child_plan != nullptr)
			delete child_plan;
	}

	Result::~Result()
	{
		for (auto p : const_quals)
			delete p;
	}

	Scan::~Scan()
	{
		for (auto p : simple_quals)
			delete p;
	}

	Join::~Join()
	{
		delete child_plan2;
	}

	AggPlan::~AggPlan()
	{
		for (auto e : groupby_list)
			delete e;
	}

	Append::~Append()
	{
		for (auto p : plan_list)
			delete p;
	}

	MergeAppend::~MergeAppend()
	{
		for (auto p : mergeplan_list)
			delete p;
	}

	Sort::~Sort()
	{
	}

	RootPlan::~RootPlan()
	{
		delete plan;
	}

	Scan::Scan(const Analyzer::RangeTblEntry &rte) : Plan()
	{
		table_id = rte.get_table_id();
		for (auto cd : rte.get_column_descs())
			col_list.push_back(cd->columnId);
	}

	RootPlan *
	Optimizer::optimize()
	{
		Plan *plan;
		SQLStmtType stmt_type = query.get_stmt_type();
		int result_table_id = 0;
		std::list<int> result_col_list;
		Analyzer::RangeTblEntry *result_rte;
		switch (stmt_type) {
			case kSELECT:
				// nothing to do for SELECT for now
				break;
			case kINSERT:
				result_rte = query.get_rangetable().front(); // the first entry is the result table
				result_table_id = result_rte->get_table_id();
				for (auto cd : result_rte->get_column_descs())
					result_col_list.push_back(cd->columnId);
				break;
			case kUPDATE:
			case kDELETE:
				// should have been rejected by the Analyzer for now
				assert(false);
				break;
			default:
				assert(false);
		}
		plan = optimize_query();
		return new RootPlan(plan, stmt_type, result_table_id, result_col_list);
	}

	Plan *
	Optimizer::optimize_query()
	{
		//@TODO add support for union queries
		if (query.get_next_query() != nullptr)
			throw std::runtime_error("UNION queries are not supported yet.");
		cur_query = &query;
		optimize_current_query();
		if (query.get_order_by() != nullptr)
			optimize_orderby();
		return cur_plan;
	}

	void
	Optimizer::optimize_current_query()
	{
		optimize_scans();
		optimize_joins();
		optimize_aggs();
		process_targetlist();
	}

	void
	Optimizer::optimize_scans()
	{
		const std::vector<Analyzer::RangeTblEntry*> &rt = cur_query->get_rangetable();
		bool first = true;
		for (auto rte : rt) {
			if (first && cur_query->get_stmt_type() == kINSERT) {
				// skip the first entry in INSERT statements which is the result table
				first = false;
				continue;
			}
			base_scans.push_back(new Scan(*rte));
		}
		const Analyzer::Expr *where_pred = cur_query->get_where_predicate();
		std::list<const Analyzer::Expr*> scan_predicates;
		if (where_pred != nullptr)
			where_pred->group_predicates(scan_predicates, join_predicates, const_predicates);
		for (auto p : scan_predicates) {
			int rte_idx;
			Analyzer::Expr *simple_pred = p->normalize_simple_predicate(rte_idx);
			if (simple_pred != nullptr)
				base_scans[rte_idx]->add_simple_predicate(simple_pred);
			else {
				std::set<int> rte_idx_set;
				p->collect_rte_idx(rte_idx_set);
				for (auto x : rte_idx_set) {
					rte_idx = x;
					break; // grab rte_idx out of the singleton set
				}
				base_scans[rte_idx]->add_predicate(p->deep_copy());
			}
		}
		const std::list<Analyzer::TargetEntry*> &tlist = cur_query->get_targetlist();
		bool(*fn_pt)(const Analyzer::ColumnVar*, const Analyzer::ColumnVar*) = Analyzer::ColumnVar::colvar_comp;
		std::set<const Analyzer::ColumnVar*, bool(*)(const Analyzer::ColumnVar*, const Analyzer::ColumnVar*)> colvar_set(fn_pt);
		for (auto tle : tlist)
			tle->get_expr()->collect_column_var(colvar_set);
		for (auto p : join_predicates)
			p->collect_column_var(colvar_set);
		const std::list<Analyzer::Expr*> *group_by = cur_query->get_group_by();
		if (group_by != nullptr)
			for (auto e : *group_by)
				e->collect_column_var(colvar_set);
		const Analyzer::Expr *having_pred = cur_query->get_having_predicate();
		if (having_pred != nullptr)
			having_pred->collect_column_var(colvar_set);
		for (auto colvar : colvar_set) {
			Analyzer::TargetEntry *tle = new Analyzer::TargetEntry("", colvar->deep_copy());
			base_scans[colvar->get_rte_idx()]->add_tle(tle);
		}
	}

	void
	Optimizer::optimize_joins()
	{
		if (base_scans.size() == 0)
			cur_plan = nullptr;
		else if (base_scans.size() == 1)
			cur_plan = base_scans[0];
		else
			throw std::runtime_error("joins are not supported yet.");
	}

	void
	Optimizer::optimize_aggs()
	{
		if (cur_query->get_num_aggs() == 0 && cur_query->get_having_predicate() == nullptr)
			return;
		std::list<Analyzer::TargetEntry*> agg_tlist;
		for (auto tle : cur_query->get_targetlist()) {
			Analyzer::TargetEntry *new_tle;
			new_tle = new Analyzer::TargetEntry(tle->get_resname(), tle->get_expr()->rewrite_with_child_targetlist(cur_plan->get_targetlist()));
			agg_tlist.push_back(new_tle);
		}
		std::list<Analyzer::Expr*> groupby_list;
		if (cur_query->get_group_by() != nullptr) {
			for (auto e : *cur_query->get_group_by()) {
				groupby_list.push_back(e->rewrite_with_child_targetlist(cur_plan->get_targetlist()));
			}
		}
		const Analyzer::Expr *having_pred = cur_query->get_having_predicate();
		std::list<Analyzer::Expr*> having_quals;
		if (having_pred != nullptr) {
			std::list<const Analyzer::Expr*> preds, others;
			having_pred->group_predicates(preds, others, others);
			assert(others.empty());
			for (auto p : preds) {
				having_quals.push_back(p->rewrite_having_clause(agg_tlist));
			}
		}
		cur_plan = new AggPlan(agg_tlist, having_quals, 0.0, cur_plan, groupby_list);
	}
	
	void
	Optimizer::optimize_orderby()
	{
		if (query.get_order_by() != nullptr)
			throw std::runtime_error("order by not supported yet.");
	}

	void
	Optimizer::process_targetlist()
	{
		std::list<Analyzer::TargetEntry*> final_tlist;
		for (auto tle : query.get_targetlist()) {
			Analyzer::TargetEntry *new_tle;
			if (cur_plan == nullptr)
				new_tle = new Analyzer::TargetEntry(tle->get_resname(), tle->get_expr()->deep_copy());
			else
				new_tle = new Analyzer::TargetEntry(tle->get_resname(), tle->get_expr()->rewrite_with_targetlist(cur_plan->get_targetlist()));
			final_tlist.push_back(new_tle);
		}
		if (cur_plan == nullptr)
			cur_plan = new ValuesScan(final_tlist);
		else {
			//delete the old TargetEntry's
			for (auto tle : cur_plan->get_targetlist())
				delete tle;
			cur_plan->set_targetlist(final_tlist);
		}
	}

	void
	Plan::print() const
	{
		std::cout << "targetlist: ";
		for (auto t : targetlist)
			t->print();
		std::cout << std::endl;
		std::cout << "quals: ";
		for (auto p : quals)
			p->print();
		std::cout << std::endl;
	}

	void
	Result::print() const
	{
		std::cout << "(Result" << std::endl;
		Plan::print();
		child_plan->print();
		std::cout << "const_quals: ";
		for (auto p : const_quals)
			p->print();
		std::cout << ")" << std::endl;
	}

	void
	Scan::print() const
	{
		std::cout << "(Scan" << std::endl;
		Plan::print();
		std::cout << "simple_quals: ";
		for (auto p : simple_quals)
			p->print();
		std::cout << std::endl << "table: " << table_id;
		std::cout << " columns: ";
		for (auto i : col_list) {
			std::cout << i;
			std::cout << " ";
		}
		std::cout << ")" << std::endl;
	}

	void
	ValuesScan::print() const
	{
		std::cout << "(ValuesScan" << std::endl;
		Plan::print();
		std::cout << ")" << std::endl;
	}

	void
	Join::print() const
	{
		std::cout << "(Join" << std::endl;
		Plan::print();
		std::cout << "Outer Plan: ";
		get_outerplan()->print();
		std::cout << "Inner Plan: ";
		get_innerplan()->print();
		std::cout << ")" << std::endl;
	}

	void
	AggPlan::print() const
	{
		std::cout << "(Agg" << std::endl;
		Plan::print();
		child_plan->print();
		std::cout << "Group By: ";
		for (auto e : groupby_list)
			e->print();
		std::cout << ")" << std::endl;
	}

	void
	Append::print() const
	{
		std::cout << "(Append" << std::endl;
		for (auto p : plan_list)
			p->print();
		std::cout << ")" << std::endl;
	}

	void
	MergeAppend::print() const
	{
		std::cout << "(MergeAppend" << std::endl;
		for (auto p : mergeplan_list)
			p->print();
		std::cout << ")" << std::endl;
	}

	void
	Sort::print() const
	{
		std::cout << "(Sort" << std::endl;
		Plan::print();
		child_plan->print();
		std::cout << "Order By: ";
		for (auto o : order_entries)
			o.print();
		std::cout << ")" << std::endl;
	}

	void
	RootPlan::print() const
	{
		std::cout << "(RootPlan ";
		switch (stmt_type) {
			case kSELECT:
				std::cout << "SELECT" << std::endl;
				break;
			case kUPDATE:
				std::cout << "UPDATE " << "result table: " << result_table_id << " columns: ";
				for (auto i : result_col_list) {
					std::cout << i;
					std::cout << " ";
				}
				std::cout << std::endl;
				break;
			case kINSERT:
				std::cout << "INSERT " << "result table: " << result_table_id << " columns: ";
				for (auto i : result_col_list) {
					std::cout << i;
					std::cout << " ";
				}
				std::cout << std::endl;
				break;
			case kDELETE:
				std::cout << "DELETE " << "result table: " << result_table_id << std::endl;
				break;
			default:
				break;
		}
		plan->print();
		std::cout << ")" << std::endl;
	}
}
