#pragma once

#include "UIDialogWnd.h"
#include "UIStatic.h"
#include "UIButton.h"
#include "UIEditBox.h"
//#include "UIListWnd.h"
#include "UIFrameWindow.h"


#include "../PhraseDialogDefs.h"

class CActor;
class CInventoryOwner;
class CPhraseDialogManager;
class CUITalkDialogWnd;

//class CUITradeWnd;
///////////////////////////////////////
//
///////////////////////////////////////

class CUITalkWnd : public CUIDialogWnd
{
private:
	typedef CUIDialogWnd inherited;
	ref_sound m_sound;
	void PlaySnd(LPCSTR text);
	void StopSnd();

public:
	CUITalkWnd();
	virtual ~CUITalkWnd();

	IC bool playing_sound() { return !!m_sound._feedback(); }
	IC CInventoryOwner* OthersInvOwner() const { return m_pOthersInvOwner; };

	void InitTalkWnd();

	virtual bool StopAnyMove() { return true; }
	virtual void SendMessage(CUIWindow* pWnd, s16 msg, void* pData = NULL);

	virtual void Draw();
	virtual void Update();

	virtual void Show(bool status);

	void Stop(); //deffered
	void StopTalk();

	void UpdateQuestions();
	void NeedUpdateQuestions();
	//инициализации начального диалога собеседника
	void InitOthersStartDialog();
	virtual bool OnKeyboardAction(int dik, EUIMessages keyboard_action);
	void SwitchToTrade();
	void SwitchToUpgrade();
	void AddIconedMessage(LPCSTR caption, LPCSTR text, LPCSTR texture_name, LPCSTR templ_name);

protected:
	//диалог
	void InitTalkDialog();
	void AskQuestion();

	void SayPhrase(const shared_str& phrase_id);

	// Функции добавления строк в листы вопросов и ответов
public:
	void AddQuestion(const shared_str& text, const shared_str& id, int number, bool b_finalizer);
	void AddAnswer(const shared_str& text, LPCSTR SpeakerName);
	bool b_disable_break;
	// Anomaly Co-op: the partner chose one of the answers this window offers (as if clicked here)
	void CoopPick(LPCSTR id);
	// Anomaly Co-op: this window as a copy of the partner's talk with the same NPC - its lines and answers come
	// from the partner's game, a chosen answer goes there; trade and upgrades stay this player's own
	bool CoopMirrorOpen(CInventoryOwner* npc);
	void CoopMirrorLine(LPCSTR speaker, LPCSTR text, bool ours);
	void CoopMirrorOptions(LPCSTR list);
	void CoopMirrorClose();
	void CoopMirrorSwitch(LPCSTR kind);
	bool CoopMirror() const { return m_coop_mirror; }
protected:
	bool m_coop_mirror = false;
	bool m_coop_picking = false;  // the partner's answer is being said here
	bool m_coop_switching = false; // this copy goes on to its own trade or upgrades
	xr_vector<std::pair<shared_str, xr_string>> m_coop_mirror_options;
	void coop_notify(LPCSTR fn, LPCSTR arg);
	xr_vector<shared_str> m_coop_options; // the ids offered now
	xr_string m_coop_options_text;        // the same with their texts: "id\ttext\n..." for the partner
protected:
	CUITalkDialogWnd* UITalkDialogWnd;

	CActor* m_pActor;
	CInventoryOwner* m_pOurInvOwner;
	CInventoryOwner* m_pOthersInvOwner;

	CPhraseDialogManager* m_pOurDialogManager;
	CPhraseDialogManager* m_pOthersDialogManager;

	bool m_bNeedToUpdateQuestions;

	//текущий диалог, если NULL, то переходим в режим выбора темы
	DIALOG_SHARED_PTR m_pCurrentDialog;
	bool TopicMode();
	void ToTopicMode();
};
