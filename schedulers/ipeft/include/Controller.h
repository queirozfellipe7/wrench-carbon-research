/**
 * Copyright (c) 2017-2018. The WRENCH Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */


#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <wrench-dev.h>
#include <map>
#include <vector>
#include <string>
#include <set>

class Controller : public wrench::ExecutionController {
public:
    Controller(const std::shared_ptr<wrench::BareMetalComputeService> &bare_metal_compute_service,
               const std::shared_ptr<wrench::SimpleStorageService> &storage_service,
               const std::string &hostname,
               const std::string &json_file_path);

    int main() override;
    std::string getWorkflowName();

    std::map<std::string, double> getHostTotalLoad() const { return host_total_load; }
    double getMakespan() const { return makespan; }

private:
    std::shared_ptr<wrench::BareMetalComputeService> bare_metal_compute_service;
    std::shared_ptr<wrench::SimpleStorageService> storage_service;
    std::string json_file_path;

    std::map<std::string, double> host_total_load;
    double makespan = 0.0;

    // Funções para IPEFT
    // FRENTE A: io_bytes/host_io_bandwidth/edge_data_bytes fazem PCT/CNCT
    // variarem por host de verdade — sem eles, a cláusula "grátis se mesmo
    // host" (Eq. 1/11/12) nunca tinha o que comparar, e PCT(t,h)/CNCT(t,h)
    // saíam idênticas pra qualquer h (confirmado por teste isolado no
    // PEFT antes desta correção ser generalizada pro IPEFT).
    std::map<std::pair<std::string, std::string>, double> compute_pct(
        const std::map<std::string, double>& comp_cost,
        const std::map<std::string, std::vector<std::string>>& successors,
        const std::vector<std::string>& host_list,
        const std::map<std::string, double>& hosts_capabilities,
        const std::vector<std::string>& task_order_reverse,
        const std::map<std::string, double>& io_bytes,
        const std::map<std::string, double>& host_io_bandwidth,
        const std::map<std::pair<std::string, std::string>, double>& edge_data_bytes
    );

    void compute_aest_alst(
        const std::map<std::string, double>& comp_cost,
        const std::map<std::string, std::vector<std::string>>& predecessors,
        const std::map<std::string, std::vector<std::string>>& successors,
        const std::vector<std::string>& host_list,
        const std::map<std::string, double>& hosts_capabilities,
        std::map<std::string, double>& aest,
        std::map<std::string, double>& alst,
        const std::vector<std::string>& task_order  // Parâmetro para escopo
    );

    // CORREÇÃO (Eq. 12, Zhou et al. 2016): pct_table foi removida dos
    // parâmetros — a versão anterior usava pct_table para a recursão
    // interna do CNCT, mas a Eq. 12 exige recursão no PRÓPRIO CNCT, não na
    // PCT (são tabelas conceitualmente diferentes). comp_cost e
    // hosts_capabilities entram no lugar, necessários para calcular
    // w(sucessor,host) diretamente, como a fórmula exige.
    // FRENTE A: io_bytes/host_io_bandwidth/edge_data_bytes, mesmo motivo
    // já documentado em compute_pct acima.
    std::map<std::pair<std::string, std::string>, double> compute_cnct(
        const std::set<std::string>& critical_nodes,
        const std::map<std::string, double>& comp_cost,
        const std::map<std::string, std::vector<std::string>>& successors,
        const std::vector<std::string>& host_list,
        const std::map<std::string, double>& hosts_capabilities,
        const std::vector<std::string>& task_order_reverse,  // ordem topológica reversa (era task_order, errado)
        const std::map<std::string, double>& io_bytes,
        const std::map<std::string, double>& host_io_bandwidth,
        const std::map<std::pair<std::string, std::string>, double>& edge_data_bytes
    );
};

#endif // CONTROLLER_H