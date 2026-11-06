#include "stdafx.h"
#include "pch_script.h"
#include "coop_props.h"
#include "UITalkWnd.h"
#include "../../xrServerEntities/script_engine.h"
#include "../ai_space.h"

#include "UITalkDialogWnd.h"

#include "../actor.h"
#include "../trade.h"
#include "../UIGameSP.h"
#include "../PDA.h"
#include "../../xrServerEntities/character_info.h"
#include "../level.h"

#include "../PhraseDialog.h"
#include "../PhraseDialogManager.h"

#include "../game_cl_base.h"
#include "../string_table.h"
#include "../xr_level_controller.h"
#include "../../xrEngine/cameraBase.h"
#include "UIXmlInit.h"
#include "UI3tButton.h"

CUITalkWnd::CUITalkWnd()
{
	m_pActor = NULL;

	m_pOurInvOwner = NULL;
	m_pOthersInvOwner = NULL;

	m_pOurDialogManager = NULL;
	m_pOthersDialogManager = NULL;

	ToTopicMode();

	InitTalkWnd();
	m_bNeedToUpdateQuestions = false;
	b_disable_break = false;
}

CUITalkWnd::~CUITalkWnd()
{
}

void CUITalkWnd::InitTalkWnd()
{
	inherited::SetWndRect(Frect().set(0, 0, UI_BASE_WIDTH, UI_BASE_HEIGHT));

	UITalkDialogWnd = xr_new<CUITalkDialogWnd>();
	UITalkDialogWnd->SetAutoDelete(true);
	AttachChild(UITalkDialogWnd);
	UITalkDialogWnd->m_pParent = this;
	UITalkDialogWnd->InitTalkDialogWnd();
}

void CUITalkWnd::InitTalkDialog()
{
	m_pActor = Actor();
	if (m_pActor && !m_pActor->IsTalking()) return;

	m_pOurInvOwner = smart_cast<CInventoryOwner*>(m_pActor);
	m_pOthersInvOwner = m_pActor->GetTalkPartner();

	m_pOurDialogManager = smart_cast<CPhraseDialogManager*>(m_pOurInvOwner);
	m_pOthersDialogManager = smart_cast<CPhraseDialogManager*>(m_pOthersInvOwner);

	//имена собеседников
	UITalkDialogWnd->UICharacterInfoLeft.InitCharacter(m_pOurInvOwner->object_id());
	UITalkDialogWnd->UICharacterInfoRight.InitCharacter(m_pOthersInvOwner->object_id());

	//.	UITalkDialogWnd->UIDialogFrame.UITitleText.SetText		(m_pOthersInvOwner->Name());
	//.	UITalkDialogWnd->UIOurPhrasesFrame.UITitleText.SetText	(m_pOurInvOwner->Name());

	//очистить лог сообщений
	UITalkDialogWnd->ClearAll();

	if (!m_coop_mirror)
		InitOthersStartDialog();
	NeedUpdateQuestions();
	Update();

	UITalkDialogWnd->mechanic_mode = m_pOthersInvOwner->SpecificCharacter().upgrade_mechanic();
	UITalkDialogWnd->SetOsoznanieMode(m_pOthersInvOwner->NeedOsoznanieMode());
	UITalkDialogWnd->Show();
	UITalkDialogWnd->UpdateButtonsLayout(b_disable_break, m_pOthersInvOwner->IsTradeEnabled());
}

void CUITalkWnd::InitOthersStartDialog()
{
	m_pOthersDialogManager->UpdateAvailableDialogs(m_pOurDialogManager);
	if (!m_pOthersDialogManager->AvailableDialogs().empty())
	{
		m_pCurrentDialog = m_pOthersDialogManager->AvailableDialogs().front();
		m_pOthersDialogManager->InitDialog(m_pOurDialogManager, m_pCurrentDialog);

		//сказать фразу
		CStringTable stbl;
		AddAnswer(m_pCurrentDialog->GetPhraseText("0"), m_pOthersInvOwner->Name());
		m_pOthersDialogManager->SayPhrase(m_pCurrentDialog, "0");

		//если диалог завершился, перейти в режим выбора темы
		if (!m_pCurrentDialog || m_pCurrentDialog->IsFinished()) ToTopicMode();
	}
}

void CUITalkWnd::NeedUpdateQuestions()
{
	m_bNeedToUpdateQuestions = true;
}

void CUITalkWnd::UpdateQuestions()
{
	UITalkDialogWnd->ClearQuestions();
	m_coop_options.clear();
	m_coop_options_text.clear();

	if (m_coop_mirror)
	{
		int number = 0;
		for (auto& o : m_coop_mirror_options)
			UITalkDialogWnd->AddQuestion(o.second.c_str(), o.first.c_str(), number++, false);
		m_bNeedToUpdateQuestions = false;
		return;
	}

	//если нет активного диалога, то
	//режима выбора темы
	if (!m_pCurrentDialog)
	{
		m_pOurDialogManager->UpdateAvailableDialogs(m_pOthersDialogManager);
		for (u32 i = 0; i < m_pOurDialogManager->AvailableDialogs().size(); ++i)
		{
			const DIALOG_SHARED_PTR& phrase_dialog = m_pOurDialogManager->AvailableDialogs()[i];
			bool bfinalizer = (phrase_dialog->GetPhrase("0"))->IsFinalizer();

			AddQuestion(phrase_dialog->DialogCaption(), phrase_dialog->GetDialogID(), i, bfinalizer);
		}
	}
	else
	{
		if (m_pCurrentDialog->IsWeSpeaking(m_pOurDialogManager))
		{
			//если в списке допустимых фраз только одна фраза пустышка, то просто
			//сказать (игрок сам не производит никаких действий)
			if (!m_pCurrentDialog->PhraseList().empty() && m_pCurrentDialog->allIsDummy())
			{
				CPhrase* phrase = m_pCurrentDialog->PhraseList()[Random.randI(m_pCurrentDialog->PhraseList().size())];
				SayPhrase(phrase->GetID());
			};

			//выбор доступных фраз из активного диалога
			if (m_pCurrentDialog && !m_pCurrentDialog->allIsDummy())
			{
				int number = 0;
				for (PHRASE_VECTOR::const_iterator it = m_pCurrentDialog->PhraseList().begin();
				     it != m_pCurrentDialog->PhraseList().end();
				     ++it, ++number)
				{
					CPhrase* phrase = *it;
					AddQuestion(m_pCurrentDialog->GetPhraseText(phrase->GetID()), phrase->GetID(), number,
					            phrase->IsFinalizer());
				}
			}
			else
				UpdateQuestions();
		}
	}
	m_bNeedToUpdateQuestions = false;

	// Anomaly Co-op: the partner's copy of this window offers the same answers (r18: sent by the engine)
	coop_props::talk_options(m_pOthersInvOwner ? m_pOthersInvOwner->Name() : "", m_coop_options_text.c_str(),
		m_pOthersInvOwner ? m_pOthersInvOwner->object_id() : u16(-1));
	::luabind::functor<void> options;
	if (ai().script_engine().functor("coop_dialog.on_options", options))
	{
		try
		{
			options(m_pOthersInvOwner ? m_pOthersInvOwner->Name() : "", m_coop_options_text.c_str(),
				m_pOthersInvOwner ? m_pOthersInvOwner->object_id() : u16(-1));
		}
		catch (...) { Msg("! coop_dialog.on_options failed"); }
	}
}

void CUITalkWnd::CoopPick(LPCSTR id)
{
	if (!IsShown() || m_coop_mirror || !id || m_bNeedToUpdateQuestions)
		return;
	shared_str pick = id;
	if (std::find(m_coop_options.begin(), m_coop_options.end(), pick) == m_coop_options.end())
		return;
	UITalkDialogWnd->m_ClickedQuestionID = pick;
	// a trade or upgrade this answer opens is the partner's: it opens there, not here
	m_coop_picking = true;
	AskQuestion();
	m_coop_picking = false;
}

void CUITalkWnd::coop_notify(LPCSTR fn, LPCSTR arg)
{
	::luabind::functor<void> f;
	if (ai().script_engine().functor(fn, f))
	{
		try { f(arg ? arg : ""); }
		catch (...) { Msg("! %s failed", fn); }
	}
}

bool CUITalkWnd::CoopMirrorOpen(CInventoryOwner* npc)
{
	CActor* actor = Actor();
	if (!actor || !npc || actor->IsTalking() || IsShown())
		return false;
	m_coop_mirror = true;
	m_coop_mirror_options.clear();
	actor->StartTalk(npc);
	if (CurrentGameUI()->TopInputReceiver())
		CurrentGameUI()->TopInputReceiver()->HideDialog();
	smart_cast<CUIGameSP*>(CurrentGameUI())->StartTalk(false);
	return true;
}

void CUITalkWnd::CoopMirrorLine(LPCSTR speaker, LPCSTR text, bool ours)
{
	if (!m_coop_mirror || !IsShown() || !text || !*text)
		return;
	PlaySnd(text);
	UITalkDialogWnd->AddAnswer(speaker, text, ours);
}

void CUITalkWnd::CoopMirrorOptions(LPCSTR list)
{
	if (!m_coop_mirror)
		return;
	m_coop_mirror_options.clear();
	// "id\ttext\n..." as the partner's window offers them
	xr_string all = list ? list : "";
	size_t at = 0;
	while (at < all.size())
	{
		size_t end = all.find('\n', at);
		if (end == xr_string::npos)
			end = all.size();
		xr_string one = all.substr(at, end - at);
		size_t tab = one.find('\t');
		if (tab != xr_string::npos)
			m_coop_mirror_options.emplace_back(shared_str(one.substr(0, tab).c_str()), one.substr(tab + 1));
		at = end + 1;
	}
	NeedUpdateQuestions();
}

void CUITalkWnd::CoopMirrorClose()
{
	if (m_coop_mirror && IsShown())
		HideDialog();
}

void CUITalkWnd::CoopMirrorSwitch(LPCSTR kind)
{
	if (!m_coop_mirror || !IsShown() || !kind)
		return;
	if (0 == xr_strcmp(kind, "upgrade"))
		SwitchToUpgrade();
	else
		SwitchToTrade();
}

//////////////////////////////////////////////////////////////////////////

void CUITalkWnd::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
	if (pWnd == UITalkDialogWnd && msg == TALK_DIALOG_TRADE_BUTTON_CLICKED)
	{
		SwitchToTrade();
	}
	else if (pWnd == UITalkDialogWnd && msg == TALK_DIALOG_UPGRADE_BUTTON_CLICKED)
	{
		SwitchToUpgrade();
	}
	else if (pWnd == UITalkDialogWnd && msg == TALK_DIALOG_QUESTION_CLICKED)
	{
		AskQuestion();
	}
	inherited::SendMessage(pWnd, msg, pData);
}

//////////////////////////////////////////////////////////////////////////
void UpdateCameraDirection(CGameObject* pTo)
{
	if (Actor()->cam_freelook != eflDisabled)
		return;

	CCameraBase* cam = Actor()->cam_Active();

	Fvector des_dir;
	Fvector des_pt;
	pTo->Center(des_pt);
	des_pt.y += pTo->Radius() * 0.5f;

	des_dir.sub(des_pt, cam->vPosition);

	float p, h;
	des_dir.getHP(h, p);


	if (angle_difference(cam->yaw, -h) > 0.2)
		cam->yaw = angle_inertion_var(cam->yaw, -h, 0.15f, 0.2f, PI_DIV_6, Device.fTimeDelta);

	if (angle_difference(cam->pitch, -p) > 0.2)
		cam->pitch = angle_inertion_var(cam->pitch, -p, 0.15f, 0.2f, PI_DIV_6, Device.fTimeDelta);
}

void CUITalkWnd::Update()
{
	//остановить разговор, если нужно
	if (g_actor && m_pActor && !m_pActor->IsTalking())
	{
		StopTalk();
	}
	else
	{
		CGameObject* pOurGO = smart_cast<CGameObject*>(m_pOurInvOwner);
		CGameObject* pOtherGO = smart_cast<CGameObject*>(m_pOthersInvOwner);

		if (NULL == pOurGO || NULL == pOtherGO)
			HideDialog();
	}

	if (m_bNeedToUpdateQuestions)
	{
		UpdateQuestions();
	}
	inherited::Update();
	UpdateCameraDirection(smart_cast<CGameObject*>(m_pOthersInvOwner));

	UITalkDialogWnd->UpdateButtonsLayout(b_disable_break, m_pOthersInvOwner->IsTradeEnabled());

	if (playing_sound())
	{
		CGameObject* pOtherGO = smart_cast<CGameObject*>(m_pOthersInvOwner);
		Fvector P = pOtherGO->Position();
		P.y += 1.8f;
		m_sound.set_position(P);
	}
}

void CUITalkWnd::Draw()
{
	inherited::Draw();
}

void CUITalkWnd::Show(bool status)
{
	inherited::Show(status);
	if (status)
	{
		InitTalkDialog();
	}
	else
	{
		StopSnd();
		UITalkDialogWnd->Hide();
		if (m_coop_mirror)
		{
			m_coop_mirror = false;
			m_coop_mirror_options.clear();
			coop_notify("coop_dialog.on_mirror_closed", m_coop_switching ? "switch" : "closed");
			m_coop_switching = false;
		}

		if (m_pActor)
		{
			ToTopicMode();

			if (m_pActor->IsTalking())
				m_pActor->StopTalk();

			m_pActor = NULL;
		}
	}
}

bool CUITalkWnd::TopicMode()
{
	return NULL == m_pCurrentDialog.get();
}

void CUITalkWnd::ToTopicMode()
{
	m_pCurrentDialog = DIALOG_SHARED_PTR((CPhraseDialog*)NULL);
}

void CUITalkWnd::AskQuestion()
{
	if (m_bNeedToUpdateQuestions) return; //quick dblclick:(
	if (m_coop_mirror)
	{
		coop_notify("coop_dialog.on_mirror_pick", UITalkDialogWnd->m_ClickedQuestionID.c_str());
		coop_props::talk_pick(UITalkDialogWnd->m_ClickedQuestionID.c_str());
		// one choice per set of answers: the next set comes with the talk's next step
		m_coop_mirror_options.clear();
		NeedUpdateQuestions();
		return;
	}
	shared_str phrase_id;

	//игрок выбрал тему разговора
	if (TopicMode())
	{
		if ((UITalkDialogWnd->m_ClickedQuestionID == "") ||
			(!m_pOurDialogManager->HaveAvailableDialog(UITalkDialogWnd->m_ClickedQuestionID)))
		{
			string128 s;
			xr_sprintf(s, "ID = [%s] of selected question is out of range of available dialogs ",
			           UITalkDialogWnd->m_ClickedQuestionID);
			VERIFY2(FALSE, s);
		}

		m_pCurrentDialog = m_pOurDialogManager->GetDialogByID(UITalkDialogWnd->m_ClickedQuestionID);

		m_pOurDialogManager->InitDialog(m_pOthersDialogManager, m_pCurrentDialog);
		phrase_id = "0";
	}
	else
	{
		phrase_id = UITalkDialogWnd->m_ClickedQuestionID;
	}

	SayPhrase(phrase_id);
	NeedUpdateQuestions();
}

void CUITalkWnd::SayPhrase(const shared_str& phrase_id)
{
	AddAnswer(m_pCurrentDialog->GetPhraseText(phrase_id), m_pOurInvOwner->Name());
	m_pOurDialogManager->SayPhrase(m_pCurrentDialog, phrase_id);
	//если диалог завершился, перейти в режим выбора темы
	if (m_pCurrentDialog->IsFinished()) ToTopicMode();
}

void CUITalkWnd::AddQuestion(const shared_str& text, const shared_str& value, int number, bool b_finalizer)
{
	if (text.size() == 0)
		return;

	UITalkDialogWnd->AddQuestion(CStringTable().translate(text).c_str(), value.c_str(), number, b_finalizer);
	m_coop_options.push_back(value);
	m_coop_options_text += xr_string(value.c_str()) + "\t" + CStringTable().translate(text).c_str() + "\n";
}

void CUITalkWnd::AddAnswer(const shared_str& text, LPCSTR SpeakerName)
{
	//для пустой фразы вообще ничего не выводим
	if (text.size() == 0)
	{
		return;
	}
	PlaySnd(text.c_str());

	bool i_am = (0 == xr_strcmp(SpeakerName, m_pOurInvOwner->Name()));
	UITalkDialogWnd->AddAnswer(SpeakerName, *CStringTable().translate(text), i_am);

	// Anomaly Co-op: the partner sees the same conversation as it goes (r18: sent by the engine)
	coop_props::talk_line(SpeakerName, *CStringTable().translate(text), m_pOthersInvOwner ? m_pOthersInvOwner->Name() : "");
	::luabind::functor<void> line;
	if (ai().script_engine().functor("coop_dialog.on_line", line))
	{
		try { line(SpeakerName, *CStringTable().translate(text), m_pOthersInvOwner ? m_pOthersInvOwner->Name() : ""); }
		catch (...) { Msg("! coop_dialog.on_line failed"); }
	}
}

void CUITalkWnd::SwitchToTrade()
{
	if (m_coop_picking)
	{
		coop_notify("coop_dialog.on_partner_switch", "trade");
		coop_props::talk_switch("trade");
		return;
	}
	if (m_coop_mirror)
		m_coop_switching = true;
	if (m_pOurInvOwner->IsTradeEnabled() && m_pOthersInvOwner->IsTradeEnabled())
	{
		CUIGameSP* pGameSP = smart_cast<CUIGameSP*>(CurrentGameUI());
		if (pGameSP)
		{
			/*			if ( pGameSP->MainInputReceiver() )
						{
							pGameSP->MainInputReceiver()->HideDialog();
						}*/
			pGameSP->StartTrade(m_pOurInvOwner, m_pOthersInvOwner);
		} // pGameSP
	}
}

void CUITalkWnd::SwitchToUpgrade()
{
	if (m_coop_picking)
	{
		coop_notify("coop_dialog.on_partner_switch", "upgrade");
		coop_props::talk_switch("upgrade");
		return;
	}
	if (m_coop_mirror)
		m_coop_switching = true;
	//if ( m_pOurInvOwner->IsInvUpgradeEnabled() && m_pOthersInvOwner->IsInvUpgradeEnabled() )
	{
		CUIGameSP* pGameSP = smart_cast<CUIGameSP*>(CurrentGameUI());
		if (pGameSP)
		{
			/*			if ( pGameSP->MainInputReceiver() )
						{
							pGameSP->MainInputReceiver()->HideDialog();
						}*/
			pGameSP->StartUpgrade(m_pOurInvOwner, m_pOthersInvOwner);
		}
	}
}

bool CUITalkWnd::OnKeyboardAction(int dik, EUIMessages keyboard_action)
{
	if (keyboard_action == WINDOW_KEY_PRESSED)
	{
		if (is_binded(kUSE, dik) || is_binded(kQUIT, dik))
		{
			if (!b_disable_break)
			{
				HideDialog();
				return true;
			}
		}
		else if (is_binded(kSPRINT_TOGGLE, dik))
		{
			if (UITalkDialogWnd->mechanic_mode)
				SwitchToUpgrade();
			else
				SwitchToTrade();
		}
	}

	return inherited::OnKeyboardAction(dik, keyboard_action);
}

void CUITalkWnd::PlaySnd(LPCSTR text)
{
	u32 text_len = xr_strlen(text);
	if (text_len == 0)
	{
		return;
	}

	string_path fn;

	LPCSTR path = "characters_voice\\dialogs\\";
	LPCSTR ext = ".ogg";
	u32 tsize = sizeof(fn) - xr_strlen(path) - xr_strlen(ext) - 1;
	if (text_len > tsize)
	{
		text_len = tsize;
	}

	strncpy_s(fn, sizeof(fn), path, xr_strlen(path));
	strncat_s(fn, sizeof(fn), text, text_len);
	strncat_s(fn, sizeof(fn), ext, xr_strlen(ext));

	//	strconcat( sizeof(fn), fn, "characters_voice\\dialogs\\", text2, ".ogg" );

	StopSnd();
	if (FS.exist("$game_sounds$", fn))
	{
		VERIFY(m_pActor);
		if (!m_pActor->OnDialogSoundHandlerStart(m_pOthersInvOwner, fn))
		{
			CGameObject* pOtherGO = smart_cast<CGameObject*>(m_pOthersInvOwner);
			Fvector P = pOtherGO->Position();
			P.y += 1.8f;
			m_sound.create(fn, st_Effect, sg_SourceType);
			m_sound.play_at_pos(0, P);
		}
	}
}

void CUITalkWnd::StopSnd()
{
	if (m_pActor && m_pActor->OnDialogSoundHandlerStop(m_pOthersInvOwner)) return;

	if (m_sound._feedback())
		m_sound.stop();
}

void CUITalkWnd::AddIconedMessage(LPCSTR caption, LPCSTR text, LPCSTR texture_name, LPCSTR templ_name)
{
	UITalkDialogWnd->AddIconedAnswer(caption, text, texture_name, templ_name);
}

void CUITalkWnd::StopTalk()
{
	HideDialog();
}

void CUITalkWnd::Stop()
{
}
