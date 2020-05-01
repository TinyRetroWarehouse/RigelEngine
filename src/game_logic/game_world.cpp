/* Copyright (C) 2019, Nikolai Wuttke. All rights reserved.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "game_world.hpp"

#include "common/game_service_provider.hpp"
#include "common/user_profile.hpp"
#include "data/game_options.hpp"
#include "data/game_traits.hpp"
#include "data/map.hpp"
#include "data/sound_ids.hpp"
#include "data/strings.hpp"
#include "data/unit_conversions.hpp"
#include "engine/entity_tools.hpp"
#include "engine/physical_components.hpp"
#include "game_logic/actor_tag.hpp"
#include "game_logic/behavior_controller.hpp"
#include "game_logic/enemies/dying_boss.hpp"
#include "game_logic/ingame_systems.hpp"
#include "game_logic/trigger_components.hpp"
#include "loader/resource_loader.hpp"
#include "renderer/upscaling_utils.hpp"
#include "ui/menu_element_renderer.hpp"
#include "ui/utils.hpp"

#include <cassert>
#include <chrono>
#include <iostream>


namespace rigel::game_logic {

using namespace engine;
using namespace std;

using data::PlayerModel;
using engine::components::WorldPosition;


namespace {

char EPISODE_PREFIXES[] = {'L', 'M', 'N', 'O'};


std::string levelFileName(const int episode, const int level) {
  assert(episode >=0 && episode < 4);
  assert(level >=0 && level < 8);

  std::string fileName;
  fileName += EPISODE_PREFIXES[episode];
  fileName += std::to_string(level + 1);
  fileName += ".MNI";
  return fileName;
}


constexpr auto BOSS_LEVEL_INTRO_MUSIC = "CALM.IMF";


struct BonusRelatedItemCounts {
  int mCameraCount = 0;
  int mFireBombCount = 0;
  int mWeaponCount = 0;
  int mMerchandiseCount = 0;
  int mBonusGlobeCount = 0;
  int mLaserTurretCount = 0;
};


BonusRelatedItemCounts countBonusRelatedItems(entityx::EntityManager& es) {
  using game_logic::components::ActorTag;
  using AT = ActorTag::Type;

  BonusRelatedItemCounts counts;

  es.each<ActorTag>([&counts](entityx::Entity, const ActorTag& tag) {
      switch (tag.mType) {
        case AT::ShootableCamera: ++counts.mCameraCount; break;
        case AT::FireBomb: ++counts.mFireBombCount; break;
        case AT::CollectableWeapon: ++counts.mWeaponCount; break;
        case AT::Merchandise: ++counts.mMerchandiseCount; break;
        case AT::ShootableBonusGlobe: ++counts.mBonusGlobeCount; break;
        case AT::MountedLaserTurret: ++counts.mLaserTurretCount; break;

        default:
          break;
      }
    });

  return counts;
}


constexpr auto HEALTH_BAR_LABEL_START_X = 0;
constexpr auto HEALTH_BAR_LABEL_START_Y = 0;
constexpr auto HEALTH_BAR_TILE_INDEX = 4*40 + 1;
constexpr auto HEALTH_BAR_START_PX = base::Vector{data::tilesToPixels(5), 0};

constexpr auto HUD_WIDTH = 6;


void drawBossHealthBar(
  const int health,
  const ui::MenuElementRenderer& textRenderer,
  const engine::TiledTexture& uiSpriteSheet
) {
  textRenderer.drawSmallWhiteText(
    HEALTH_BAR_LABEL_START_X, HEALTH_BAR_LABEL_START_Y, "BOSS");

  const auto healthBarSize = base::Extents{health, data::GameTraits::tileSize};
  uiSpriteSheet.renderTileStretched(
    HEALTH_BAR_TILE_INDEX, {HEALTH_BAR_START_PX, healthBarSize});
}


auto asVec(const base::Size<int>& size) {
  return base::Vector{size.width, size.height};
}


auto asSize(const base::Vector& vec) {
  return base::Size{vec.x, vec.y};
}


auto scaleVec(const base::Vector& vec, const base::Point<float>& scale) {
  return base::Vector{
    base::round(vec.x * scale.x),
    base::round(vec.y * scale.y)};
}


auto localToGlobalTranslation(
  const renderer::Renderer* pRenderer,
  const base::Vector& translation
) {
  return pRenderer->globalTranslation() +
    scaleVec(translation, pRenderer->globalScale());
}


[[nodiscard]] auto setupIngameViewport(
  renderer::Renderer* pRenderer,
  const int screenShakeOffsetX
) {
  auto saved = renderer::Renderer::StateSaver{pRenderer};

  const auto offset = data::GameTraits::inGameViewPortOffset +
    base::Vector{screenShakeOffsetX, 0};
  const auto newTranslation = localToGlobalTranslation(pRenderer, offset);

  const auto scale = pRenderer->globalScale();
  pRenderer->setClipRect(base::Rect<int>{
    newTranslation,
    asSize(scaleVec(asVec(data::GameTraits::inGameViewPortSize), scale))});
  pRenderer->setGlobalTranslation(newTranslation);

  return saved;
}


[[nodiscard]] auto setupIngameViewportWidescreen(
  renderer::Renderer* pRenderer,
  const renderer::WidescreenViewPortInfo& info,
  const int screenShakeOffsetX
) {
  auto saved = renderer::Renderer::StateSaver{pRenderer};

  const auto scale = pRenderer->globalScale();
  const auto offset = base::Vector{
    screenShakeOffsetX, data::GameTraits::inGameViewPortOffset.y};
  const auto newTranslation =
    scaleVec(offset, scale) + base::Vector{info.mLeftPaddingPx, 0};
  pRenderer->setGlobalTranslation(newTranslation);

  const auto viewPortSize = base::Extents{
    info.mWidthPx,
    base::round(data::GameTraits::inGameViewPortSize.height * scale.y)
  };
  pRenderer->setClipRect(base::Rect<int>{newTranslation, viewPortSize});

  return saved;
}


void setupWidescreenHudOffset(
  renderer::Renderer* pRenderer,
  const renderer::WidescreenViewPortInfo& info
) {
  const auto extraTiles =
    info.mWidthTiles - data::GameTraits::mapViewPortWidthTiles;
  const auto hudOffset = (extraTiles - HUD_WIDTH) * data::GameTraits::tileSize;
  pRenderer->setGlobalTranslation(
    localToGlobalTranslation(pRenderer, {hudOffset, 0}));
}


[[nodiscard]] auto setupWidescreenTopRowViewPort(
  renderer::Renderer* pRenderer,
  const renderer::WidescreenViewPortInfo& info
) {
  auto saved = renderer::Renderer::StateSaver{pRenderer};

  pRenderer->setGlobalTranslation({
    info.mLeftPaddingPx, pRenderer->globalTranslation().y});
  pRenderer->setClipRect({});
  return saved;
}


std::vector<base::Vector> collectRadarDots(
  entityx::EntityManager& entities,
  const base::Vector& playerPosition
) {
  using engine::components::Active;
  using engine::components::WorldPosition;
  using game_logic::components::AppearsOnRadar;

  std::vector<base::Vector> radarDots;

  entities.each<WorldPosition, AppearsOnRadar, Active>(
    [&](
      entityx::Entity,
      const WorldPosition& position,
      const AppearsOnRadar&,
      const Active&
    ) {
      const auto positionRelativeToPlayer = position - playerPosition;
      if (ui::isVisibleOnRadar(positionRelativeToPlayer)) {
        radarDots.push_back(positionRelativeToPlayer);
      }
    });

  return radarDots;
}

}


GameWorld::WorldState::WorldState(
  entityx::EventManager& eventManager,
  SpriteFactory* pSpriteFactory,
  data::GameSessionId sessionId
)
  : mEntities(eventManager)
  , mEntityFactory(
    pSpriteFactory,
    &mEntities,
    &mRandomGenerator,
    sessionId.mDifficulty)
  , mRadarDishCounter(mEntities, eventManager)
{
}


GameWorld::WorldState::~WorldState() = default;


GameWorld::GameWorld(
  data::PlayerModel* pPlayerModel,
  const data::GameSessionId& sessionId,
  GameMode::Context context,
  std::optional<base::Vector> playerPositionOverride,
  bool showWelcomeMessage
)
  : mpRenderer(context.mpRenderer)
  , mpServiceProvider(context.mpServiceProvider)
  , mpUiSpriteSheet(context.mpUiSpriteSheet)
  , mpTextRenderer(context.mpUiRenderer)
  , mpPlayerModel(pPlayerModel)
  , mpOptions(&context.mpUserProfile->mOptions)
  , mSpriteFactory(context.mpRenderer, &context.mpResources->mActorImagePackage)
  , mPlayerModelAtLevelStart(*mpPlayerModel)
  , mHudRenderer(
      sessionId.mLevel + 1,
      mpRenderer,
      *context.mpResources,
      context.mpUiSpriteSheet)
  , mMessageDisplay(mpServiceProvider, context.mpUiRenderer)
  , mState(mEventManager, &mSpriteFactory, sessionId)
{
  mEventManager.subscribe<rigel::events::CheckPointActivated>(*this);
  mEventManager.subscribe<rigel::events::ExitReached>(*this);
  mEventManager.subscribe<rigel::events::PlayerDied>(*this);
  mEventManager.subscribe<rigel::events::PlayerTookDamage>(*this);
  mEventManager.subscribe<rigel::events::PlayerMessage>(*this);
  mEventManager.subscribe<rigel::events::PlayerTeleported>(*this);
  mEventManager.subscribe<rigel::events::ScreenFlash>(*this);
  mEventManager.subscribe<rigel::events::ScreenShake>(*this);
  mEventManager.subscribe<rigel::events::TutorialMessage>(*this);
  mEventManager.subscribe<rigel::game_logic::events::ShootableKilled>(*this);
  mEventManager.subscribe<rigel::events::BossActivated>(*this);
  mEventManager.subscribe<rigel::events::BossDestroyed>(*this);

  using namespace std::chrono;
  auto before = high_resolution_clock::now();

  loadLevel(sessionId, *context.mpResources);

  if (playerPositionOverride) {
    mState.mpSystems->player().position() = *playerPositionOverride;
  }

  mState.mpSystems->centerViewOnPlayer();

  updateGameLogic({});

  if (showWelcomeMessage) {
    mMessageDisplay.setMessage(data::Messages::WelcomeToDukeNukem2);
  }

  if (mState.mEarthQuakeEffect) { // overrides welcome message
    showTutorialMessage(data::TutorialMessageId::EarthQuake);
  }

  if (mState.mRadarDishCounter.radarDishesPresent()) { // overrides earth quake
    mMessageDisplay.setMessage(data::Messages::FindAllRadars);
  }

  auto after = high_resolution_clock::now();
  std::cout << "Level load time: " <<
    duration<double>(after - before).count() * 1000.0 << " ms\n";
}


GameWorld::~GameWorld() = default;


bool GameWorld::levelFinished() const {
  return mState.mLevelFinished;
}


std::set<data::Bonus> GameWorld::achievedBonuses() const {
  std::set<data::Bonus> bonuses;

  if (!mState.mBonusInfo.mPlayerTookDamage) {
    bonuses.insert(data::Bonus::NoDamageTaken);
  }

  const auto counts =
    countBonusRelatedItems(
      const_cast<entityx::EntityManager&>(mState.mEntities));

  if (mState.mBonusInfo.mInitialCameraCount > 0 && counts.mCameraCount == 0) {
    bonuses.insert(data::Bonus::DestroyedAllCameras);
  }

  // NOTE: This is a bug (?) in the original game - if a level doesn't contain
  // any fire bombs, bonus 6 will be awarded, as if the player had destroyed
  // all fire bombs.
  if (counts.mFireBombCount == 0) {
    bonuses.insert(data::Bonus::DestroyedAllFireBombs);
  }

  if (
    mState.mBonusInfo.mInitialMerchandiseCount > 0 && counts.mMerchandiseCount == 0
  ) {
    bonuses.insert(data::Bonus::CollectedAllMerchandise);
  }

  if (mState.mBonusInfo.mInitialWeaponCount > 0 && counts.mWeaponCount == 0) {
    bonuses.insert(data::Bonus::CollectedEveryWeapon);
  }

  if (
    mState.mBonusInfo.mInitialLaserTurretCount > 0 && counts.mLaserTurretCount == 0
  ) {
    bonuses.insert(data::Bonus::DestroyedAllSpinningLaserTurrets);
  }

  if (mState.mBonusInfo.mInitialBonusGlobeCount == mState.mBonusInfo.mNumShotBonusGlobes) {
    bonuses.insert(data::Bonus::ShotAllBonusGlobes);
  }

  return bonuses;
}


void GameWorld::receive(const rigel::events::CheckPointActivated& event) {
  mActivatedCheckpoint = CheckpointData{
    mpPlayerModel->makeCheckpoint(), event.mPosition};
  mMessageDisplay.setMessage(data::Messages::FoundRespawnBeacon);
}


void GameWorld::receive(const rigel::events::ExitReached& event) {
  if (mState.mRadarDishCounter.radarDishesPresent() && event.mCheckRadarDishes) {
    showTutorialMessage(data::TutorialMessageId::RadarsStillFunctional);
  } else {
    mState.mLevelFinished = true;
  }
}


void GameWorld::receive(const rigel::events::PlayerDied& event) {
  mState.mPlayerDied = true;
}


void GameWorld::receive(const rigel::events::PlayerTookDamage& event) {
  mState.mBonusInfo.mPlayerTookDamage = true;
}


void GameWorld::receive(const rigel::events::PlayerMessage& event) {
  mMessageDisplay.setMessage(event.mText);
}


void GameWorld::receive(const rigel::events::PlayerTeleported& event) {
  mState.mTeleportTargetPosition = event.mNewPosition;
}


void GameWorld::receive(const rigel::events::ScreenFlash& event) {
  mState.mScreenFlashColor = event.mColor;
}


void GameWorld::receive(const rigel::events::ScreenShake& event) {
  mState.mScreenShakeOffsetX = event.mAmount;
}


void GameWorld::receive(const rigel::events::TutorialMessage& event) {
  showTutorialMessage(event.mId);
}


void GameWorld::receive(const game_logic::events::ShootableKilled& event) {
  using game_logic::components::ActorTag;
  auto entity = event.mEntity;
  if (!entity.has_component<ActorTag>()) {
    return;
  }

  const auto& position = *entity.component<const WorldPosition>();

  using AT = ActorTag::Type;
  const auto type = entity.component<ActorTag>()->mType;
  switch (type) {
    case AT::Reactor:
      onReactorDestroyed(position);
      break;

    case AT::ShootableBonusGlobe:
      ++mState.mBonusInfo.mNumShotBonusGlobes;
      break;

    default:
      break;
  }
}


void GameWorld::receive(const rigel::events::BossActivated& event) {
  mState.mActiveBossEntity = event.mBossEntity;
  mpServiceProvider->playMusic(*mState.mLevelMusicFile);
}


void GameWorld::receive(const rigel::events::BossDestroyed& event) {
  mState.mBossDeathAnimationStartPending = true;
}


void GameWorld::loadLevel(
  const data::GameSessionId& sessionId,
  const loader::ResourceLoader& resources
) {
  auto loadedLevel = loader::loadLevel(
    levelFileName(sessionId.mEpisode, sessionId.mLevel),
    resources,
    sessionId.mDifficulty);
  auto playerEntity =
    mState.mEntityFactory.createEntitiesForLevel(loadedLevel.mActors);

  const auto counts = countBonusRelatedItems(mState.mEntities);
  mState.mBonusInfo.mInitialCameraCount = counts.mCameraCount;
  mState.mBonusInfo.mInitialMerchandiseCount = counts.mMerchandiseCount;
  mState.mBonusInfo.mInitialWeaponCount = counts.mWeaponCount;
  mState.mBonusInfo.mInitialLaserTurretCount = counts.mLaserTurretCount;
  mState.mBonusInfo.mInitialBonusGlobeCount = counts.mBonusGlobeCount;

  mState.mMap = std::move(loadedLevel.mMap);
  mState.mInitialActors = std::move(loadedLevel.mActors);
  mState.mBackdropSwitchCondition = loadedLevel.mBackdropSwitchCondition;
  mState.mMapAtLevelStart = mState.mMap;

  mState.mpSystems = std::make_unique<IngameSystems>(
    sessionId,
    playerEntity,
    mpPlayerModel,
    &mState.mMap,
    engine::MapRenderer::MapRenderData{std::move(loadedLevel)},
    mpServiceProvider,
    &mState.mEntityFactory,
    &mState.mRandomGenerator,
    &mState.mRadarDishCounter,
    mpRenderer,
    mState.mEntities,
    mEventManager,
    resources);

  if (loadedLevel.mEarthquake) {
    mState.mEarthQuakeEffect = EarthQuakeEffect{
      mpServiceProvider, &mState.mRandomGenerator, &mEventManager};
  }

  if (data::isBossLevel(sessionId.mLevel)) {
    mState.mLevelMusicFile = loadedLevel.mMusicFile;
    mpServiceProvider->playMusic(BOSS_LEVEL_INTRO_MUSIC);
  } else {
    mpServiceProvider->playMusic(loadedLevel.mMusicFile);
  }
}


void GameWorld::updateGameLogic(const PlayerInput& input) {
  mState.mBackdropFlashColor = std::nullopt;
  mState.mScreenFlashColor = std::nullopt;

  if (mState.mReactorDestructionFramesElapsed) {
    updateReactorDestructionEvent();
  }

  if (mState.mEarthQuakeEffect) {
    mState.mEarthQuakeEffect->update();
  }

  mHudRenderer.updateAnimation();
  mMessageDisplay.update();

  if (mState.mActiveBossEntity && mState.mBossDeathAnimationStartPending) {
    engine::removeSafely<game_logic::components::PlayerDamaging>(
      mState.mActiveBossEntity);
    mState.mActiveBossEntity.replace<game_logic::components::BehaviorController>(
      behaviors::DyingBoss{});
    mState.mBossDeathAnimationStartPending = false;
  }

  if (
    mpOptions->mWidescreenModeOn && renderer::canUseWidescreenMode(mpRenderer)
  ) {
    const auto info = renderer::determineWidescreenViewPort(mpRenderer);
    const auto viewPortSize = base::Extents{
      info.mWidthTiles - HUD_WIDTH,
      data::GameTraits::mapViewPortSize.height};
    mState.mpSystems->update(input, mState.mEntities, viewPortSize);
  } else {
    mState.mpSystems->update(input, mState.mEntities, data::GameTraits::mapViewPortSize);
  }
}


void GameWorld::render() {
  const auto widescreenModeOn =
    mpOptions->mWidescreenModeOn && renderer::canUseWidescreenMode(mpRenderer);

  auto drawWorld = [this](const base::Extents& viewPortSize) {
    if (!mState.mScreenFlashColor) {
      mState.mpSystems->render(
        mState.mEntities, mState.mBackdropFlashColor, viewPortSize);
    } else {
      mpRenderer->clear(*mState.mScreenFlashColor);
    }
  };

  auto drawTopRow = [&, this]() {
    if (mState.mActiveBossEntity) {
      using game_logic::components::Shootable;

      const auto health = mState.mActiveBossEntity.has_component<Shootable>()
        ? mState.mActiveBossEntity.component<Shootable>()->mHealth : 0;

      if (widescreenModeOn) {
        drawBossHealthBar(health, *mpTextRenderer, *mpUiSpriteSheet);
      } else {
        auto saved = renderer::Renderer::StateSaver{mpRenderer};
        mpRenderer->setGlobalTranslation(localToGlobalTranslation(
          mpRenderer, {data::GameTraits::tileSize, 0}));

        drawBossHealthBar(health, *mpTextRenderer, *mpUiSpriteSheet);
      }
    } else {
      mMessageDisplay.render();
    }
  };

  auto drawHud = [&, this]() {
    const auto radarDots =
      collectRadarDots(mState.mEntities, mState.mpSystems->player().position());
    mHudRenderer.render(*mpPlayerModel, radarDots);
  };


  if (widescreenModeOn) {
    const auto info = renderer::determineWidescreenViewPort(mpRenderer);

    {
      const auto saved = setupIngameViewportWidescreen(
        mpRenderer, info, mState.mScreenShakeOffsetX);

      drawWorld({info.mWidthTiles, data::GameTraits::viewPortHeightTiles});

      setupWidescreenHudOffset(mpRenderer, info);
      drawHud();
    }

    auto saved = setupWidescreenTopRowViewPort(mpRenderer, info);
    drawTopRow();
  } else {
    {
      const auto saved = setupIngameViewport(mpRenderer, mState.mScreenShakeOffsetX);

      drawWorld(data::GameTraits::mapViewPortSize);
      drawHud();
    }

    drawTopRow();
  }
}


void GameWorld::processEndOfFrameActions() {
  handlePlayerDeath();
  handleLevelExit();
  handleTeleporter();

  mState.mScreenShakeOffsetX = 0;
}


void GameWorld::onReactorDestroyed(const base::Vector& position) {
  mState.mScreenFlashColor = loader::INGAME_PALETTE[7];
  mState.mEntityFactory.createProjectile(
    ProjectileType::ReactorDebris,
    position + base::Vector{-1, 0},
    ProjectileDirection::Left);
  mState.mEntityFactory.createProjectile(
    ProjectileType::ReactorDebris,
    position + base::Vector{3, 0},
    ProjectileDirection::Right);

  const auto shouldDoSpecialEvent =
    mState.mBackdropSwitchCondition ==
      data::map::BackdropSwitchCondition::OnReactorDestruction;
  if (!mState.mReactorDestructionFramesElapsed && shouldDoSpecialEvent) {
    mState.mpSystems->switchBackdrops();
    mState.mBackdropSwitched = true;
    mState.mReactorDestructionFramesElapsed = 0;
  }
}


void GameWorld::updateReactorDestructionEvent() {
  auto& framesElapsed = *mState.mReactorDestructionFramesElapsed;
  if (framesElapsed >= 14) {
    return;
  }

  if (framesElapsed == 13) {
    mMessageDisplay.setMessage(data::Messages::DestroyedEverything);
  } else if (framesElapsed % 2 == 1) {
    mState.mBackdropFlashColor = base::Color{255, 255, 255, 255};
    mpServiceProvider->playSound(data::SoundId::BigExplosion);
  }

  ++framesElapsed;
}


void GameWorld::handleLevelExit() {
  using engine::components::Active;
  using engine::components::BoundingBox;
  using game_logic::components::Trigger;
  using game_logic::components::TriggerType;

  mState.mEntities.each<Trigger, WorldPosition, Active>(
    [this](
      entityx::Entity,
      const Trigger& trigger,
      const WorldPosition& triggerPosition,
      const Active&
    ) {
      if (trigger.mType != TriggerType::LevelExit || mState.mLevelFinished) {
        return;
      }

      const auto playerBBox = mState.mpSystems->player().worldSpaceHitBox();
      const auto playerAboveOrAtTriggerHeight =
        playerBBox.bottom() <= triggerPosition.y;
      const auto touchingTriggerOnXAxis =
        triggerPosition.x >= playerBBox.left() &&
        triggerPosition.x <= (playerBBox.right() + 1);

      const auto triggerActivated =
        playerAboveOrAtTriggerHeight && touchingTriggerOnXAxis;

      if (triggerActivated) {
        mEventManager.emit(rigel::events::ExitReached{});
      }
    });
}


void GameWorld::handlePlayerDeath() {
  if (mState.mPlayerDied) {
    mState.mPlayerDied = false;
    mState.mActiveBossEntity = {};

    if (mActivatedCheckpoint) {
      restartFromCheckpoint();
    } else {
      restartLevel();
    }
  }
}


void GameWorld::restartLevel() {
  mpServiceProvider->fadeOutScreen();

  if (mState.mBackdropSwitched) {
    mState.mpSystems->switchBackdrops();
    mState.mBackdropSwitched = false;
  }

  mState.mMap = mState.mMapAtLevelStart;
  mState.mBonusInfo.mNumShotBonusGlobes = 0;
  mState.mBonusInfo.mPlayerTookDamage = false;

  mState.mEntities.reset();
  auto playerEntity = mState.mEntityFactory.createEntitiesForLevel(
    mState.mInitialActors);
  mState.mpSystems->restartFromBeginning(playerEntity);

  *mpPlayerModel = mPlayerModelAtLevelStart;

  mState.mpSystems->centerViewOnPlayer();
  render();

  mpServiceProvider->fadeInScreen();
}


void GameWorld::restartFromCheckpoint() {
  mpServiceProvider->fadeOutScreen();

  const auto shouldSwitchBackAfterRespawn =
    mState.mBackdropSwitchCondition ==
      data::map::BackdropSwitchCondition::OnTeleportation;
  if (mState.mBackdropSwitched && shouldSwitchBackAfterRespawn) {
    mState.mpSystems->switchBackdrops();
    mState.mBackdropSwitched = false;
  }

  mpPlayerModel->restoreFromCheckpoint(mActivatedCheckpoint->mState);
  mState.mpSystems->restartFromCheckpoint(mActivatedCheckpoint->mPosition);

  mState.mpSystems->centerViewOnPlayer();
  render();

  mpServiceProvider->fadeInScreen();
}


void GameWorld::handleTeleporter() {
  if (!mState.mTeleportTargetPosition) {
    return;
  }

  mpServiceProvider->fadeOutScreen();

  mState.mpSystems->player().position() = *mState.mTeleportTargetPosition;
  mState.mTeleportTargetPosition = std::nullopt;

  const auto switchBackdrop =
    mState.mBackdropSwitchCondition ==
      data::map::BackdropSwitchCondition::OnTeleportation;
  if (switchBackdrop) {
    mState.mpSystems->switchBackdrops();
    mState.mBackdropSwitched = !mState.mBackdropSwitched;
  }

  mState.mpSystems->centerViewOnPlayer();
  render();
  mpServiceProvider->fadeInScreen();
}


void GameWorld::showTutorialMessage(const data::TutorialMessageId id) {
  if (!mpPlayerModel->tutorialMessages().hasBeenShown(id)) {
    mMessageDisplay.setMessage(data::messageText(id));
    mpPlayerModel->tutorialMessages().markAsShown(id);
  }
}


void GameWorld::printDebugText(std::ostream& stream) const {
  mState.mpSystems->printDebugText(stream);
  stream << "Entities: " << mState.mEntities.size() << '\n';
}

}
