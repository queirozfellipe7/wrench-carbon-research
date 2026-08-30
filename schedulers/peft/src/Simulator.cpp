// Arquivo: src/Simulator.cpp

#include <iostream>
#include <vector>
#include <iomanip>
#include <wrench-dev.h>

// Necessário para funções de energia do SimGrid
#include <simgrid/plugins/energy.h>

#include "Controller.h"
#include "host_carbon_footprint.h"

int main(int argc, char **argv) {
    auto simulation = wrench::Simulation::createSimulation();
    simulation->init(&argc, argv);

    // Inicializa plugins necessários
    sg_host_energy_plugin_init();              // Plugin de energia do SimGrid
    sg_host_carbon_footprint_plugin_init();    // Seu plugin customizado de carbono

    std::cout << "INFO: Plugins de energia e pegada de carbono inicializados." << std::endl;

    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <xml platform file> <workflow json file>" << std::endl;
        return 1;
    }

    // Carrega a plataforma
    simulation->instantiatePlatform(argv[1]);
    std::string workflow_file = argv[2];

    // Serviço de armazenamento no host principal
    auto storage_service = simulation->add(
        wrench::SimpleStorageService::createSimpleStorageService("DGX_System", {"/"}, {}, {})
    );

    // Lista de hosts que podem executar computação — lida dinamicamente
    // da plataforma carregada em tempo de execução (argv[1]), em vez de
    // hardcoded. Isso permite trocar entre platform_8gpu.xml,
    // platform_16gpu.xml, platform_32gpu.xml etc. sem precisar recompilar.
    std::vector<std::string> compute_hosts;
    for (auto const &host : simgrid::s4u::Engine::get_instance()->get_all_hosts()) {
        // Ignora VMs, caso existam (mesmo critério já usado mais abaixo no arquivo)
        if (dynamic_cast<simgrid::s4u::VirtualMachine*>(host) != nullptr) continue;
        // DGX_System é ORQUESTRADOR + ARMAZENAMENTO (roda o Controller e detém
        // o disco), não um executor de kernels: ele NÃO entra como host de
        // computação, para não competir com as GPUs por tarefas. As decisões
        // de escalonamento passam a ser exclusivamente entre os aceleradores.
        if (std::string(host->get_cname()) == "DGX_System") continue;
        compute_hosts.push_back(host->get_cname());
    }

    std::cout << "INFO: " << compute_hosts.size()
              << " host(s) de computação detectado(s) na plataforma: ";
    for (auto const &h : compute_hosts) std::cout << h << " ";
    std::cout << std::endl;

    // Serviço Bare-Metal Compute
    auto baremetal_service = simulation->add(
        new wrench::BareMetalComputeService("DGX_System", compute_hosts, "", {})
    );

    // Controller (escalonador HEFT + coleta de métricas)
    auto controller = simulation->add(
        new Controller(baremetal_service, storage_service, "DGX_System", workflow_file)
    );

    // Executa a simulação
    simulation->launch();

    std::cout << "INFO: Simulação concluída." << std::endl;

    // =========================================================================
    //                RESULTADOS – ENERGIA, CARBONO E EFICIÊNCIA
    // =========================================================================

    std::cout << "\n";
    std::cout << std::string(70, '=') << std::endl;
    std::cout << "         RESULTADOS DE ENERGIA, CARBONO E UTILIZAÇÃO" << std::endl;
    std::cout << std::string(70, '=') << std::endl;

    auto host_list = simgrid::s4u::Engine::get_instance()->get_all_hosts();

    double total_energy_joules = 0.0;
    double total_carbon_gco2   = 0.0;
    int    num_physical_hosts  = 0;

    std::cout << std::left
              << std::setw(18) << "Host"
              << std::setw(14) << "Energia (J)"
              << std::setw(14) << "Energia (kWh)"
              << std::setw(14) << "Carbono (gCO₂)"
              << std::endl;
    std::cout << std::string(70, '-') << std::endl;

    for (auto const& host : host_list) {
        // Ignora VMs (consumo é contabilizado no host físico)
        if (dynamic_cast<simgrid::s4u::VirtualMachine*>(host) != nullptr) {
            continue;
        }

        // ENERGIA ANALITICA (Caminho 2): coerente com o makespan do modelo,
        // no lugar do plugin do SimGrid (que media sobre o tempo fisico orfao).
        // E = pidle*makespan + (pmax-pidle)*ativo. Potencia lida do XML (nunca
        // hardcoded); ativo vem do merge de intervalos (sem dupla contagem).
        static auto host_load_energy = controller->getHostTotalLoad();
        static double makespan_energy = controller->getMakespan();
        double pidle = 0.0, pmax = 0.0, ci = 473.0;
        const char* w = host->get_property("wattage_per_state");
        if (w) { std::string ws(w); auto cc = ws.find(':');
            try { if (cc!=std::string::npos){ pidle=std::stod(ws.substr(0,cc)); pmax=std::stod(ws.substr(cc+1)); } } catch(...){} }
        const char* cip = host->get_property("carbon_intensity");
        if (cip) { try { ci = std::stod(cip); } catch(...){} }
        std::string hnome = host->get_cname();
        double ativo = host_load_energy.count(hnome) ? host_load_energy[hnome] : 0.0;
        double energy_j = pidle * makespan_energy + (pmax - pidle) * ativo;
        double carbon   = (energy_j / 3.6e6) * ci;

        double energy_kwh = energy_j / 3.6e6;

        total_energy_joules += energy_j;
        total_carbon_gco2   += carbon;
        num_physical_hosts++;

        std::cout << std::left
                  << std::setw(18) << host->get_cname()
                  << std::fixed << std::setprecision(0) << std::setw(14) << energy_j
                  << std::fixed << std::setprecision(4) << std::setw(14) << energy_kwh
                  << std::fixed << std::setprecision(2) << std::setw(14) << carbon
                  << std::endl;
    }

    double total_sim_time = simgrid::s4u::Engine::get_clock();
    double total_energy_kwh = total_energy_joules / 3.6e6;

    // Utilização REAL da plataforma, calculada a partir dos dados que o
    // Controller já registrou durante a execução (host_total_load / makespan).
    // Substitui o valor fixo de 42.5% que existia aqui antes.
    double utilization_percent = 0.0;
    {
        auto host_load = controller->getHostTotalLoad();
        double makespan = controller->getMakespan();

        if (!host_load.empty() && makespan > 0) {
            double sum_ratio = 0.0;
            for (auto const &h : host_load) {
                sum_ratio += h.second / makespan;   // fração de tempo ocupado, por host
            }
            utilization_percent = (sum_ratio / host_load.size()) * 100.0;
        }
    }

    // Custo operacional estimado (ajuste o valor do kWh conforme sua realidade)
    double preco_kwh_reais = 0.85;  // R$/kWh – exemplo médio Brasil 2024–2025
    double custo_estimado = total_energy_kwh * preco_kwh_reais;

    std::cout << std::string(70, '-') << std::endl;

    std::cout << std::left << std::setw(35) << "Tempo total simulado:"
              << std::fixed << std::setprecision(2) << total_sim_time << " segundos" << std::endl;

    std::cout << std::left << std::setw(35) << "Número de hosts físicos:"
              << num_physical_hosts << std::endl;

    std::cout << std::left << std::setw(35) << "Energia total consumida:"
              << std::fixed << std::setprecision(3) << total_energy_kwh << " kWh" << std::endl;

    std::cout << std::left << std::setw(35) << "Pegada de carbono total:"
              << std::fixed << std::setprecision(1) << total_carbon_gco2 << " gCO₂" << std::endl;

    std::cout << std::left << std::setw(35) << "Utilização média (aprox.):"
              << std::fixed << std::setprecision(1) << utilization_percent << " %" << std::endl;

    std::cout << std::left << std::setw(35) << "Custo operacional estimado:"
              << "R$ " << std::fixed << std::setprecision(2) << custo_estimado << std::endl;

    std::cout << std::string(70, '=') << std::endl;

    return 0;
}