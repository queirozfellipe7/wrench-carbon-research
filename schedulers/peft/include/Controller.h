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

    // Função para computar OCT (Eq. 7, Arabnejad & Barbosa 2014).
    // FRENTE A (completa): io_bytes/host_io_bandwidth fazem o custo de
    // tarefas de I/O variar por host; edge_data_bytes é o tamanho real de
    // dado transferido em cada aresta do DAG (cruzando saída do pai com
    // entrada do filho, via "uses"/file_reads/file_writes + files) — é o
    // que permite a cláusula "ci,j=0 se mesmo host" (Eq. 7) finalmente
    // fazer OCT(t,h) variar por host de verdade, não só o custo de I/O
    // isoladamente (que sozinho não bastava — validado por teste isolado).
    std::map<std::pair<std::string, std::string>, double> compute_oct(
        const std::map<std::string, double>& comp_cost,
        const std::map<std::string, std::vector<std::string>>& successors,
        const std::vector<std::string>& host_list,
        const std::map<std::string, double>& hosts_capabilities,
        const std::vector<std::string>& task_order_reverse,  // ordem topológica reversa
        const std::map<std::string, double>& io_bytes,
        const std::map<std::string, double>& host_io_bandwidth,
        const std::map<std::pair<std::string, std::string>, double>& edge_data_bytes
    );
};

#endif // CONTROLLER_H