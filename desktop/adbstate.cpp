#include "adbstate.h"

#include <cctype>
#include <cstdlib>

namespace cable {

int parse_version_code(const std::string &t)
{
	if (t.find("Unable to find package") != std::string::npos) return kVersionAbsent;
	// adb's own failures ("error: device offline", "adb: no devices/emulators found"...) are not "absent".
	size_t pos = 0;
	while (pos < t.size()) {
		size_t end = t.find('\n', pos);
		if (end == std::string::npos) end = t.size();
		if (t.compare(pos, 6, "error:") == 0 || t.compare(pos, 5, "adb: ") == 0) return kVersionError;
		pos = end + 1;
	}
	size_t from = t.find("Package [io.gbahandoff]");
	if (from == std::string::npos) from = 0;
	size_t v = t.find("versionCode=", from);
	if (v == std::string::npos) return kVersionAbsent;
	v += 12;
	if (v >= t.size() || !isdigit((unsigned char)t[v])) return kVersionAbsent;
	long n = strtol(t.c_str() + v, nullptr, 10);
	return n < 0 || n > 0x7FFFFFFF ? kVersionAbsent : (int)n;
}

PhoneState classify_app(int installed, int expected)
{
	if (installed < 0) return PhoneState::NoApp;
	return installed < expected ? PhoneState::Outdated : PhoneState::Ready;
}

InstallResult parse_install_result(int rc, const std::string &out)
{
	InstallResult r;
	if (rc == -1) { r.message = "Nao foi possivel executar o adb."; return r; }
	if (rc == -2) { r.message = "Tempo esgotado ao instalar. Confira o celular e tente de novo."; return r; }
	if (rc == 0 && out.find("Success") != std::string::npos) { r.ok = true; return r; }
	static const char kNoUninstall[] = " NAO desinstale o app (apagaria seu save): peca ajuda.";
	size_t f = out.find("Failure [");
	std::string code;
	if (f != std::string::npos) {
		size_t b = f + 9, e = out.find_first_of(":]", b);
		code = out.substr(b, e == std::string::npos ? std::string::npos : e - b);
	}
	if (code == "INSTALL_FAILED_UPDATE_INCOMPATIBLE")
		r.message = "A assinatura do app instalado e diferente." + std::string(kNoUninstall);
	else if (code == "INSTALL_FAILED_USER_RESTRICTED" || code == "INSTALL_FAILED_VERIFICATION_FAILURE")
		r.message = "O celular recusou. Ative 'Instalar via USB' nas opcoes do desenvolvedor e aceite no aparelho.";
	else if (code == "INSTALL_FAILED_INSUFFICIENT_STORAGE")
		r.message = "Sem espaco no celular. Libere espaco e tente de novo (NAO desinstale este app).";
	else if (code == "INSTALL_FAILED_VERSION_DOWNGRADE")
		r.message = "O celular tem uma versao mais nova do app." + std::string(kNoUninstall);
	else if (!code.empty())
		r.message = "Falha ao instalar (" + code + ")." + std::string(kNoUninstall);
	else
		r.message = "Falha ao instalar." + std::string(kNoUninstall);
	return r;
}

} // namespace cable
