#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "app/app.hpp"
#include "render/cell.hpp"

namespace sshos {

// Monotone, jamais réutilisé. Un identifiant recyclé ferait qu'une action
// différée (une fermeture, un événement de descripteur en retard) frappe
// une fenêtre qui n'est pas celle qu'on visait.
using WindowId = uint32_t;

enum class WinMode { Normal, Minimized, Maximized, Fullscreen };

struct Window {
  WindowId id = 0;
  std::string title;

  // L'identifiant de catalogue dont cette fenêtre est née. Le panneau s'en
  // sert pour donner le focus à une application déjà ouverte plutôt que
  // d'en lancer une seconde.
  std::string app_id;

  // user_rect est la géométrie VOULUE, en mode Normal, jamais écrasée par
  // une contrainte d'affichage. display_rect en est la projection dans la
  // zone de travail du moment. C'est cette séparation qui rend le
  // redimensionnement du terminal réversible : rétrécir puis rétablir
  // rend exactement la disposition d'origine.
  Rect user_rect{};
  Rect display_rect{};

  WinMode mode = WinMode::Normal;
  WinMode before_fullscreen = WinMode::Normal;

  // ORDRE SIGNIFICATIF : les membres sont détruits dans l'ordre inverse de
  // leur déclaration. `app` doit mourir avant `host`, sinon une
  // application qui appelle host->unwatch() dans son destructeur (voir
  // Battement) déréférence un hôte déjà détruit.
  std::unique_ptr<Host> host;
  std::unique_ptr<App> app;

  // Dernière taille annoncée à l'application. Sert à n'appeler on_resize()
  // que lorsqu'elle change réellement.
  Size sent_size{-1, -1};

  bool close_requested = false;
};

Rect client_rect(const Rect& frame);
Size frame_min(const App& app);
Rect clamp_to(Rect frame, const Rect& work, Size min);

}  // namespace sshos
