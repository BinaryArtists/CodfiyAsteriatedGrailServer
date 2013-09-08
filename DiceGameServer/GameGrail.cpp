#include "stdafx.h"

#include <boost/random.hpp>
#include <boost/bind.hpp>
#include "GameGrail.h"
#include "GrailState.h"
#include "zLogger.h"
#include "zCommonDefine.h"
#include "UserSessionManager.h"
using namespace boost;

void TeamArea::initialTeam()
{
    this->moraleBLUE = 15;
    this->moraleRED = 15;
    this->gemBLUE = 0;
    this->gemRED = 0;
    this->crystalBLUE = 0;
    this->crystalRED = 0;
    this->cupBLUE = 0;
    this->cupRED = 0;
}

void TeamArea::setCrystal(int color, int value)
{
    if(color == RED && value+gemRED<=5)
        this->crystalRED = value;
    else if(color == BLUE && value+gemBLUE<=5)
        this->crystalBLUE = value;
}

void TeamArea::setCup(int color, int value)
{
    if(color == RED)
        this->cupRED = value;
    else if(color == BLUE)
        this->cupBLUE = value;
}

void TeamArea::setGem(int color, int value)
{
    if(color == RED && value+crystalRED<=5)
        this->gemRED = value;
    else if(color == BLUE && value+crystalBLUE<=5)
        this->gemBLUE = value;
}

void TeamArea::setMorale(int color, int value)
{
    if(value<0)
        value=0;
    if(color == RED)
        this->moraleRED = value;
    else if(color == BLUE)
        this->moraleBLUE = value;

}

GameGrail::GameGrail(GameGrailConfig *config)
{
	m_gameId = config->getTableId();
	m_gameName = config->getTableName();
	m_gameType = GAME_TYPE_GRAIL;
	m_roundId = 0;
	m_maxPlayers = config->maxPlayers;
	m_roleStrategy = config->roleStrategy;
	m_seatMode = 0;
	m_responseTime = 10;
	m_maxAttempts = 2;
	pushGameState(new StateWaitForEnter);
}

int GameGrail::popGameState_if(int state)
{
	if(topGameState()->state == state){
		delete m_states.top(); 
		m_states.pop();
		return GE_SUCCESS;
	}
	ztLoggerWrite(ZONE, e_Warning, "[Table %d] Attempt to pop state: %d, but current state: %d"
		, m_gameId, state, topGameState()->state);
	return GE_INVALID_PLAYERID;
}

void GameGrail::sendMessage(int id, string msg)
{
#ifdef Debug
	ztLoggerWrite(ZONE, e_Debug, "[Table %d] send to %d, string: %s, size: %d", m_gameId, id, msg.c_str(), msg.size());
#endif
	UserTask *ref;
	PlayerContextList::iterator it;
	if(id == -1){
		for(it = m_playerContexts.begin(); it !=m_playerContexts.end(); it++){
			ref = UserSessionManager::getInstance().getUser(it->second->getUserId());
			if(ref){
				ref->SendCmd(msg);
			}
			else{
				ztLoggerWrite(ZONE, e_Debug, "[Table %d] Cannot find UserSession, PlayerID: %d, UserID: %s", m_gameId, id, it->second->getUserId().c_str());
			}
		}
	}
	else{
		it = m_playerContexts.find(id);
		if(it != m_playerContexts.end()){
			ref = UserSessionManager::getInstance().getUser(it->second->getUserId());
			if(ref){
				ref->SendCmd(msg);
			}
			else{
				ztLoggerWrite(ZONE, e_Debug, "[Table %d] Cannot find UserSession, PlayerID: %d, UserID: %s", m_gameId, id, it->second->getUserId().c_str());
			}
		}
	}
}

bool GameGrail::isReady(int id)
{
	if(id == -1){
		for(int i = 0; i < m_maxPlayers; i++){
			if(!m_ready[i]){
				return false;
			}
		}
		return true;
	}
	else if(id >= 0 && id < m_maxPlayers){
		return m_ready[id];
	}
	ztLoggerWrite(ZONE, e_Error, "invalid player id: %d", id);
	return false;
} 

bool GameGrail::waitForOne(int id, string msg, int sec, bool toResetReady)
{
	if(id < 0 || id >= m_maxPlayers){
		ztLoggerWrite(ZONE, e_Error, "unvaliad player id: %d", id);
		return false;
	}
	m_token = id;
	if(toResetReady){
		resetReady(id);
	}	
	
	int attempts = 0;
	boost::mutex::scoped_lock lock(m_mutex_for_wait);
	while(attempts < m_maxAttempts)
	{
		sendMessage(-1, msg);
		boost::system_time const timeout=boost::get_system_time()+ boost::posix_time::milliseconds(sec*1000);
		if(m_condition_for_wait.timed_wait(lock,timeout,boost::bind( &GameGrail::isReady, this, id )))
			return true;
		attempts++;
	}
	return false;
}

bool GameGrail::waitForAll(string* msgs, int sec, bool toResetReady)
{
	m_token = -1;
	int attempts = 0;
	boost::mutex::scoped_lock lock(m_mutex_for_wait);
	if(toResetReady){
		resetReady();
	}
	while(attempts < m_maxAttempts)
	{
		for(int i = 0; i < m_maxPlayers; i++){
			if(!m_ready[i]){
				sendMessage(i, msgs[i]);
			}
		}
		boost::system_time const timeout=boost::get_system_time()+ boost::posix_time::milliseconds(sec*1000);
		if(m_condition_for_wait.timed_wait(lock,timeout,boost::bind( &GameGrail::isReady, this, -1 ))){
			return true;
		}
		attempts++;
	}
	return false;
}

bool GameGrail::tryNotify(int id, int state, int step, void* reply)
{
	boost::mutex::scoped_lock lock(m_mutex_for_notify);
	if(id == m_token && state == topGameState()->state && step == topGameState()->step) {
		m_ready[id] = true;
		if(reply){
			m_playerContexts[id]->setBuf(reply);
		}
		m_condition_for_wait.notify_one();
		return true;
	}
	else if(m_token == -1 && state == topGameState()->state && step == topGameState()->step){
		m_ready[id] = true;
		if(reply){
			m_playerContexts[id]->setBuf(reply);
		}
		if(isReady(-1)){
			m_condition_for_wait.notify_one();
		}
	    return true;
	}
	ztLoggerWrite(ZONE, e_Warning, "[Table %d] Unauthorized notify detected. Player id: %d, current token: %d; claimed state: %d, current state: %d; claim step: %d, current step: %d", m_gameId,
		id, m_token, state, topGameState()->state, step, topGameState()->step);
	return false;
}

int GameGrail::getReply(int id, void* &reply)
{
	std::map<int, GameGrailPlayerContext*>::iterator iter;
	if((iter=m_playerContexts.find(id)) == m_playerContexts.end()){
		return GE_INVALID_PLAYERID;
	}
	reply = iter->second->getBuf();
	if(!reply){
		return GE_NO_REPLY;
	}	
	return GE_SUCCESS;
}

//FIXME: �ֽ׶�ֻ֧��Ҳֻ��֧�����������
//���ƶѡ�����״̬������������������
//�����Ƴ������ƶѡ�����״̬�������������
int GameGrail::setStateMoveCards(int srcOwner, int srcArea, int dstOwner, int dstArea, int howMany, vector< int > cards, bool isShown, HARM* harm)
{
	PlayerEntity *src, *dst;
	HARM doHarm;
	int ret;
	//check whether exists
	switch(srcArea)
	{
	case DECK_PILE:
		//no need to check
		break;
	case DECK_HAND:
		src = getPlayerEntity(srcOwner);
		if(GE_SUCCESS != (ret=src->checkHandCards(howMany,cards))){
			throw ret;
		}
		break;
	case DECK_BASIC_EFFECT:
		src = getPlayerEntity(srcOwner);
		//FIXME up to now 1 is enough
		if(howMany != 1){
			return GE_NOT_SUPPORTED;
		}
		if(GE_SUCCESS != (ret = src->checkBasicEffect(cards[0]))){
			return ret;
		}
		break;
	default:
		return GE_NOT_SUPPORTED;
	}
	//FIXME should use two message instead of one moveCardNotice 
	sendMessage(-1, Coder::moveCardNotice(howMany, cards, srcOwner, srcArea, dstOwner, dstArea));
	//src hand change->show hand->dst hand change->dst hand overload, but stack is LIFO
	switch(dstArea)
	{
	case DECK_DISCARD:
		ret = discard->push(howMany,&cards[0]);
		break;
	case DECK_HAND:
		dst = getPlayerEntity(dstOwner);
		ret = dst->addHandCards(howMany,cards);
		if(!harm){
			doHarm.type = HARM_NONE;
		}
		else{
			doHarm = *harm;
		}
		ret = setStateHandOverLoad(dstOwner, doHarm);
		pushGameState(new StateHandChange(dstOwner));							
		break;
	case DECK_BASIC_EFFECT:
		dst = getPlayerEntity(dstOwner);
		if(howMany != 1){
			return GE_NOT_SUPPORTED;
		}
		ret = dst->addBasicEffect(cards[0],srcOwner);
		//TODO: another state? ��ʹ�
		break;
	case DECK_COVER:
		dst = getPlayerEntity(dstOwner);
		ret = dst->addCoverCards(howMany, cards);
		//TODO: cover overload
		//ret = setStateCoverOverLoad(dstOwner);
		break;
	default:
		return GE_NOT_SUPPORTED;
	}
	switch(srcArea)
	{
	case DECK_HAND:
		src->removeHandCards(howMany, cards);
		if(isShown){
			pushGameState(new StateShowHand(srcOwner, howMany, cards));
		}
		pushGameState(new StateHandChange(srcOwner));
		break;
	case DECK_BASIC_EFFECT:
		src->removeBasicEffect(cards[0]);
		break;
	}
	return GE_SUCCESS;
}

int GameGrail::simpleMoveCards(int srcOwner, int srcArea, int dstOwner, int dstArea, int howMany, vector< int > cards)
{
	int ret = GE_MOVECARD_FAILED;
	PlayerEntity *src, *dst;
	//FIXME up to now DECK_DISCARD is not covered here
	switch(srcArea)
	{
	case DECK_BASIC_EFFECT:
		src = getPlayerEntity(srcOwner);		
		if(!src){
			return GE_INVALID_PLAYERID;
		}
		//FIXME up to now 1 is enough
		if(howMany != 1){
			return GE_NOT_SUPPORTED;
		}
		ret = src->removeBasicEffect(cards[0]);
		break;
	case DECK_COVER:
		src = getPlayerEntity(srcOwner);
		if(!src){
			return GE_INVALID_PLAYERID;
		}
		ret = src->removeCoverCards(howMany,cards);
		break;		
	default:
		return GE_NOT_SUPPORTED;
	}
	sendMessage(-1, Coder::moveCardNotice(howMany, cards, srcOwner, srcArea, dstOwner, dstArea));	
		
	if(dstArea == DECK_DISCARD){
		ret = discard->push(howMany,&cards[0]);
	}
	else{
		return GE_NOT_SUPPORTED;
	}
	return ret;
}

int GameGrail::setStateMoveOneCard(int srcOwner, int srcArea, int dstOwner, int dstArea, int cardID, bool isShown, HARM* harm)
{
	vector< int > wrapper(1);
	wrapper[0] = cardID;
	return setStateMoveCards(srcOwner, srcArea, dstOwner, dstArea, 1, wrapper, isShown, harm);
}

int GameGrail::drawCardsFromPile(int howMany, vector< int > &cards)
{	
	int out[CARDBUF];
	//�ƶѺ���
	if(GE_SUCCESS != pile->pop(howMany,out)){
		int temp[CARDSUM];
		int outPtr;
		int pilePtr;
		outPtr = pile->popAll(out);
		pilePtr = discard->popAll(temp);
		pile->push(pilePtr, temp);
		pile->randomize();
		if(!pile->pop(howMany-outPtr,out+outPtr)){
			ztLoggerWrite(ZONE, e_Error, "[Table %d] Running out of cards.", m_gameId);
			return GE_CARD_NOT_ENOUGH;
		}
	}
	cards = vector< int >(out, out+howMany);
	return GE_SUCCESS;
}

//Must ensure it is called through last line and must push the nextState first
int GameGrail::setStateDrawCardsToHand(int howMany, int playerID, HARM *harm)
{
	int ret;
	vector< int >cards;
	ret = drawCardsFromPile(howMany, cards);
	sendMessage(-1, Coder::drawNotice(playerID, howMany, cards));
	setStateMoveCards(-1, DECK_PILE, playerID, DECK_HAND, howMany, cards, harm);
	return ret;
}

//Must ensure it is called through last line and must push the nextState first
int GameGrail::setStateHandOverLoad(int dstID, HARM harm)
{
	PlayerEntity *dst = getPlayerEntity(dstID);
	if(!dst){
		return GE_INVALID_PLAYERID;
	}
	int overNum = dst->getHandCardNum() - dst->getHandCardMax();
	if (overNum <= 0){
		return GE_SUCCESS;
	}
	pushGameState(new StateDiscardHand(dstID,overNum, harm, true, false));
	return GE_SUCCESS;
}

int GameGrail::setStateUseCard(int cardID, int dstID, int srcID, bool realCard)
{
	if(realCard){
		sendMessage(-1,Coder::useCardNotice(cardID,dstID,srcID));
		return setStateMoveOneCard(srcID, DECK_HAND, -1, DECK_DISCARD, cardID, true);
	}
	else{
		sendMessage(-1,Coder::useCardNotice(cardID,dstID,srcID,0));
		return GE_SUCCESS;
	}
}

int GameGrail::setStateCheckBasicEffect()
{
	//TODO check weaken here
	int cardID = 0;	
	if(false)
	{
		//FIXME ��ϵ��������
		int howMany = 3;		
		pushGameState(new StateWeaken(howMany, cardID));
	}
	else
	{
		pushGameState(new StateBeforeAction);
	}
	//TODO: push timeline3 states here based on basicEffect
	return GE_SUCCESS;
}

int GameGrail::setStateAttackAction(int cardID, int dstID, int srcID, bool realCard)
{
	pushGameState(new StateAfterAttack(cardID, dstID, srcID));	
	setStateTimeline1(cardID, dstID, srcID, true);
	pushGameState(new StateBeforeAttack(cardID, dstID, srcID));	
	return setStateUseCard(cardID, dstID, srcID, realCard);
}

int GameGrail::setStateReattack(int attackFromCard, int attackToCard, int attackFrom, int attacked , int attackTo, bool isActive, bool realCard)
{
	setStateTimeline1(attackToCard, attackTo, attacked, false);
	setStateTimeline2Miss(attackFromCard, attacked, attackFrom, isActive);	
	return setStateUseCard(attackToCard, attackTo, attacked, realCard);
}

int GameGrail::setStateAttackGiveUp(int cardID, int dstID, int srcID, HARM harm, bool isActive)
{
	//TODO check sheild here
	if(false){
		return setStateTimeline2Miss(cardID, dstID, srcID, isActive);		
	}
	else{
		return setStateTimeline2Hit(cardID, dstID, srcID, harm, isActive);
	}
}

int GameGrail::setStateTimeline1(int cardID, int dstID, int srcID, bool isActive)
{
	CONTEXT_TIMELINE_1* con = new CONTEXT_TIMELINE_1;
	con->attack.cardID = cardID;
	con->attack.srcID = srcID;
	con->attack.dstID = dstID;
	con->attack.isActive = isActive;
	con->harm.srcID = srcID;
	con->harm.point = 2;
	con->harm.type = HARM_ATTACK;	
	//FIXME: ����
	con->hitRate = RATE_NORMAL;
	pushGameState(new StateTimeline1(con));
	return GE_SUCCESS;
}

int GameGrail::setStateTimeline2Miss(int cardID, int dstID, int srcID, bool isActive)
{
	CONTEXT_TIMELINE_2_MISS* con = new CONTEXT_TIMELINE_2_MISS;
	con->cardID = cardID;
	con->srcID = srcID;
	con->dstID = dstID;
	con->isActive = isActive;
	pushGameState(new StateTimeline2Miss(con));
	return GE_SUCCESS;
}

int GameGrail::setStateTimeline2Hit(int cardID, int dstID, int srcID, HARM harm, bool isActive)
{
	sendMessage(-1, Coder::hitNotice(1, isActive, dstID, srcID));
	//TODO stones
	CONTEXT_TIMELINE_2_HIT *con = new CONTEXT_TIMELINE_2_HIT;
	con->attack.cardID = cardID;
	con->attack.dstID = dstID;
	con->attack.srcID = srcID;
	con->attack.isActive = isActive;
	con->harm = harm;
	pushGameState(new StateTimeline2Hit(con));
	return GE_SUCCESS;
}

int GameGrail::setStateTimeline3(int dstID, HARM harm)
{
	CONTEXT_TIMELINE_3 *con = new CONTEXT_TIMELINE_3;
	con->harm = harm;
	con->dstID = dstID;
	pushGameState(new StateTimeline3(con));
	return GE_SUCCESS;
}

int GameGrail::setStateTrueLoseMorale(int howMany, int dstID, HARM harm)
{
	CONTEXT_LOSE_MORALE *con = new CONTEXT_LOSE_MORALE;
	con->dstID = dstID;
	con->harm = harm;
	con->howMany = howMany;
	pushGameState(new StateTrueLoseMorale(con));
	return GE_SUCCESS;
}

int GameGrail::setStateCheckTurnEnd()
{
	PlayerEntity *player = getPlayerEntity(m_currentPlayerID);
	if(!player){
		return GE_INVALID_PLAYERID;
	}
	if(player->getHasAdditionalAction())
	{
		//TODO: additional action state
	}
	else
	{
		pushGameState(new StateTurnEnd);
	}
	return GE_SUCCESS;
}

PlayerEntity* GameGrail::getPlayerEntity(int playerID)
{
	if(playerID<0 || playerID>m_maxPlayers){
		ztLoggerWrite(ZONE, e_Error, "[Table %d] Invalid PlayerID: %d", 
					m_gameId, playerID);
		throw GE_INVALID_PLAYERID;
	}
	return m_playerEntities[playerID]; 
}

void GameGrail::GameRun()
{
	ztLoggerWrite(ZONE, e_Information, "GameGrail::GameRun() GameGrail [%d] %s create!!", 
					m_gameId, m_gameName.c_str());
	int ret;
	GrailState* currentState;
	while(true)
	{
		try{
			ret = GE_NO_STATE;
			currentState = topGameState();
			if(currentState){
				ret = currentState->handle(this);
			}
			if(ret != GE_SUCCESS){
				ztLoggerWrite(ZONE, e_Error, "[Table %d] Handle returns error: %d. Current state: %d", 
					m_gameId, ret, currentState->state);
			}
		}catch(int error)
		{
			ztLoggerWrite(ZONE, e_Error, "[Table %d] Handle throws error: %d. Current state: %d", 
				m_gameId, error, topGameState()->state);
		}
	}
}

int GameGrail::playerEnterIntoTable(GameGrailPlayerContext *player)
{
	if(isCanSitIntoTable())
	{
		int availableID;
		for(availableID=0;availableID<m_maxPlayers;availableID++)
		{
			if(m_playerContexts.find(availableID) == m_playerContexts.end())
			{
				UserSession *ref = UserSessionManager::getInstance().getUser(player->getUserId());
				ref->setPlayerID(availableID);
				m_playerContexts.insert(PlayerContextList::value_type(availableID, player));
				string temp = "1;";    
                temp += TOQSTR(availableID);
                temp += ";";
				sendMessage(availableID, temp);
				return 0;
			}
		}
	}
	return 1;
}

int GameGrail::setStateRoleStrategy()
{
	switch (m_roleStrategy)
	{
	case ROLE_STRATEGY_RANDOM:	
		pushGameState(new StateRoleStrategyRandom);
		break;
	case ROLE_STRATEGY_31:
		pushGameState(new GrailState(STATE_ROLE_STRATEGY_31));
		break;
	case ROLE_STRATEGY_BP:
		pushGameState(new GrailState(STATE_ROLE_STRATEGY_BP));
		break;
	default:
		return GE_INVALID_ARGUMENT;
	}
	return GE_SUCCESS;
}

int GameGrail::setStateCurrentPlayer(int playerID)
{
	if(playerID<0 || playerID>m_maxPlayers){
		return GE_INVALID_ARGUMENT;
	}
	PlayerEntity *player = getPlayerEntity(playerID);
	if(!player){
		return GE_INVALID_ARGUMENT;
	}
	player->setHasAdditionalAction(false);
	m_currentPlayerID = playerID;
	pushGameState(new StateBeforeTurnBegin);
	return GE_SUCCESS;
}

Deck* GameGrail::initRoles()
{
	Deck *roles;
	roles = new Deck(30);
	roles->init(1, 24);
	int temp[]={26, 28, 29};
	roles->push(3, temp);
	roles->randomize();
	return roles;
}

void GameGrail::initPlayerEntities()
{
	int pre;
	int id;
	int post;
	int color;
	if(m_maxPlayers<2){
		ztLoggerWrite(ZONE, e_Error, "[Table %d] maxPlayers: %d must be at least 2", 
					m_gameId, m_maxPlayers);
		return;
	}
	//FIXME should init roles instead of playerEntity
	for(int i = 0; i < m_maxPlayers; i++){
		id = queue[i] - '0';
		color = queue[i+m_maxPlayers] - '0';
		m_playerEntities[id] = new PlayerEntity(this, id, color);
	}
	for(int i = 1; i < m_maxPlayers-1; i++){
		id = queue[i] - '0';
		post = queue[i+1] - '0';
		pre = queue[i-1] - '0';
		m_playerEntities[id]->setPost(m_playerEntities[post]);
		m_playerEntities[id]->setPre(m_playerEntities[pre]);
	}
	post = queue[0] - '0';
	id = queue[m_maxPlayers-1] - '0';
	pre = queue[m_maxPlayers-2] - '0';
	m_playerEntities[id]->setPost(m_playerEntities[post]);
	m_playerEntities[id]->setPre(m_playerEntities[pre]);

	post = queue[1] - '0';
	id = queue[0] - '0';
	pre = queue[m_maxPlayers-1] - '0';
	m_playerEntities[id]->setPost(m_playerEntities[post]);
	m_playerEntities[id]->setPre(m_playerEntities[pre]);
	m_teamArea = new TeamArea;
}
/*
int GameGrail::handleRoleStrategy31(GrailState *state)
{
	ztLoggerWrite(ZONE, e_Debug, "[Table %d] Enter handleRoleStrategy31", m_gameId);
	CONTEXT_BROADCAST* con;
	if(!state->context){
		Deck *roles = initRoles();
		con = new CONTEXT_BROADCAST;
		int out[3];
		for(int i = 0; i < m_maxPlayers; i++){
			if(roles->pop(3, out)){
				con->msgs[i] = Coder::askForRolePick(3, out);
			}
			else{
				return GE_INVALID_ARGUMENT;
			}
		}
		state->context = con;
		delete roles;
	}
	con = (CONTEXT_BROADCAST*)state->context;
	if(waitForAll(con->msgs)){
		//TODO
		;
	}
	else{
		return GE_TIMEOUT;
	}
}

*/
