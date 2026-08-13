#include "vt/reply.hpp"

namespace sshos {

std::string device_attributes() { return "\033[?62;22c"; }

std::string secondary_device_attributes() { return "\033[>1;20;0c"; }

std::string cursor_position_report(int x, int y) {
  // Le fil compte à partir de UN, la grille à partir de zéro. Se tromper
  // d'une unité place le curseur d'un `vim` une ligne trop haut.
  return "\033[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "R";
}

std::string extended_cursor_position_report(int x, int y) {
  // La page est toujours la 1 : nous n'en avons qu'une.
  return "\033[?" + std::to_string(y + 1) + ";" + std::to_string(x + 1) +
         ";1R";
}

std::string device_status_ok() { return "\033[0n"; }

std::string dec_mode_report(int mode, const Modes& modes) {
  // 0 non reconnu, 1 posé, 2 éteint. « Non reconnu » n'est PAS « éteint » :
  // un invité qui lirait « éteint » pour un mode que nous n'avons pas
  // croirait pouvoir l'allumer, et attendrait un effet qui ne viendra pas.
  int state = 0;
  if (modes.knows(mode)) state = modes.get(mode) ? 1 : 2;
  return "\033[?" + std::to_string(mode) + ";" + std::to_string(state) + "$y";
}

std::string reply_for_csi(const Params& params, std::string_view intermediates,
                          uint8_t final_byte, int cursor_x, int cursor_y,
                          const Modes& modes) {
  if (final_byte == 'c') {
    // `CSI c` et `CSI 0 c` demandent la même chose. Un marqueur privé
    // qu'on ne connaît pas, en revanche, n'est pas une demande d'identité :
    // répondre à tort mettrait des octets dans le fil de quelqu'un qui
    // n'attendait rien.
    if (intermediates.empty()) return device_attributes();
    if (intermediates == ">") return secondary_device_attributes();
    return "";
  }

  if (final_byte == 'n' && intermediates.empty()) {
    switch (param_or(params, 0, 0)) {
      case 5:
        return device_status_ok();
      case 6:
        return cursor_position_report(cursor_x, cursor_y);
      default:
        return "";
    }
  }

  // `CSI ? 6 n` est une AUTRE question : la position ETENDUE, qui porte en
  // plus le numero de page. Y repondre la forme ordinaire donnerait a
  // l'invite une reponse qu'il n'attend pas, au mauvais format ; ne pas y
  // repondre du tout le laisserait bloque. Nous n'avons qu'une page, et
  // c'est la page 1.
  if (final_byte == 'n' && intermediates == "?" &&
      param_or(params, 0, 0) == 6) {
    return extended_cursor_position_report(cursor_x, cursor_y);
  }

  if (final_byte == 'p') {
    // Un `$ p` sans paramètre ne désigne aucun mode : il n'y a rien à dire.
    const int mode = param_or(params, 0, -1);
    if (mode < 0) return "";
    if (intermediates == "?$") return dec_mode_report(mode, modes);
    if (intermediates == "$") {
      // Mode ANSI. Nous n'en gérons aucun -- mais il faut le DIRE, pas se
      // taire : l'invité attend une réponse.
      return "\033[" + std::to_string(mode) + ";0$y";
    }
  }

  return "";
}

}  // namespace sshos
