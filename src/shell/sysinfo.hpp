#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "apps/monitor/procstat.hpp"
#include "render/surface.hpp"
#include "render/theme.hpp"

namespace sshos {

// LE MONITEUR, EN FOND D'ÉCRAN.
//
// Il a d'abord été une application, avec sa fenêtre à empiler et à
// retrouver. C'est une mauvaise place pour lui : on veut voir la charge de
// la machine EN MÊME TEMPS qu'on travaille, pas à la place. Il vit donc
// sur le bureau lui-même, derrière les fenêtres, sur la moitié droite --
// là où l'on ne pose jamais rien en premier.
//
// Quatre sections séparées, dans cet ordre : le processeur, la mémoire, le
// réseau, puis les processus. Les trois premières tiennent en quelques
// lignes et répondent à « la machine souffre-t-elle ? » ; la quatrième
// répond à « à cause de qui ? », et c'est elle qu'on tronque quand la
// place manque.
class SysInfo {
 public:
  // Lit `/proc` si la seconde est écoulée. `now_ms` vient de l'appelant :
  // c'est ce qui rend l'échantillonnage vérifiable sans attendre.
  void refresh(int64_t now_ms);

  // Dessine dans `v`, qui est DÉJÀ la moitié droite : le widget ne décide
  // pas où il va, il remplit ce qu'on lui donne.
  void draw(View v, const Theme& th, Border b) const;

  // LA SIGNATURE DU BUREAU, en lettres de blocs. Elle vit sur la moitie
  // GAUCHE -- celle que le widget laisse libre -- dans une teinte proche
  // du fond : presente, mais qui ne crie pas, et qui disparait derriere
  // les fenetres des qu'on travaille.
  static void draw_banner(View v, const Theme& th, Border b);


  // Empeche TOUTE lecture de `/proc`. Pose une seule fois par le binaire
  // de test : le widget lit la vraie machine -- charge, memoire, nombre de
  // coeurs, processus vivants -- et aucune reference de rendu ne peut etre
  // stable tant qu'il le fait.
  static void freeze_for_tests();

  // --- pour les tests ---
  void sample_for_tests(int64_t now_ms, std::string_view stat,
                        std::string_view meminfo, std::string_view loadavg,
                        std::string_view netdev,
                        const std::vector<ProcInfo>& procs);
  const std::vector<int>& cores_for_tests() const { return cores_; }
  uint64_t rx_rate_for_tests() const { return rx_per_s_; }
  uint64_t tx_rate_for_tests() const { return tx_per_s_; }
  const std::vector<ProcRow>& rows_for_tests() const { return rows_; }

 private:
  void apply(int64_t now_ms, std::string_view stat, std::string_view meminfo,
             std::string_view loadavg, std::string_view netdev,
             const std::vector<ProcInfo>& procs);

  std::vector<CpuTimes> prev_cpu_;
  std::vector<ProcInfo> prev_procs_;
  NetTotals prev_net_;
  bool sampled_ = false;
  int64_t last_ms_ = 0;

  std::vector<int> cores_;
  MemInfo mem_;
  std::vector<int> load_{0, 0, 0};
  uint64_t rx_per_s_ = 0;
  uint64_t tx_per_s_ = 0;
  std::vector<ProcRow> rows_;
};

}  // namespace sshos
