/* Copyright (C) 2016, Nikolai Wuttke. All rights reserved.
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

#include "damage_infliction_system.hpp"

#include "data/player_data.hpp"
#include "engine/base_components.hpp"
#include "engine/physical_components.hpp"
#include "game_logic/damage_components.hpp"
#include "game_logic/dynamic_geometry_components.hpp"
#include "game_mode.hpp"


namespace rigel { namespace game_logic {

namespace ex = entityx;

using engine::components::BoundingBox;
using engine::components::CollidedWithWorld;
using engine::components::WorldPosition;
using game_logic::components::DamageInflicting;
using game_logic::components::MapGeometryLink;
using game_logic::components::Shootable;


DamageInflictionSystem::DamageInflictionSystem(
  data::PlayerModel* pPlayerModel,
  data::map::Map* pMap,
  IGameServiceProvider* pServiceProvider
)
  : mpPlayerModel(pPlayerModel)
  , mpMap(pMap)
  , mpServiceProvider(pServiceProvider)
{
}


void DamageInflictionSystem::update(
  ex::EntityManager& es,
  ex::EventManager& events,
  ex::TimeDelta dt
) {
  es.each<DamageInflicting, WorldPosition, BoundingBox>(
    [this, &es](
      ex::Entity inflictorEntity,
      const DamageInflicting& damage,
      const WorldPosition& inflictorPosition,
      const BoundingBox& bbox
    ) {
      const auto inflictorBbox = engine::toWorldSpace(bbox, inflictorPosition);

      ex::ComponentHandle<Shootable> shootable;
      ex::ComponentHandle<WorldPosition> shootablePos;
      ex::ComponentHandle<BoundingBox> shootableBboxLocal;
      for (auto shootableEntity : es.entities_with_components(
        shootable, shootablePos, shootableBboxLocal)
      ) {
        const auto shootableBbox =
          engine::toWorldSpace(*shootableBboxLocal, *shootablePos);
        if (
          shootableBbox.intersects(inflictorBbox) &&
          !shootable->mInvincible
        ) {
          inflictorEntity.destroy();

          shootable->mHealth -= damage.mAmount;
          if (shootable->mHealth <= 0) {
            mpPlayerModel->mScore += shootable->mGivenScore;

            // Take care of shootable walls
            // NOTE: This might move someplace else later - maybe.
            if (shootableEntity.has_component<MapGeometryLink>()) {
              const auto mapSection =
                shootableEntity.component<MapGeometryLink>()
                  ->mLinkedGeometrySection;
              mpMap->clearSection(
                mapSection.topLeft.x, mapSection.topLeft.y,
                mapSection.size.width, mapSection.size.height);
            }

            // Generate sound and destroy entity
            // NOTE: This is only temporary, it will change once we implement
            // different sounds and particle effects per enemy/object.
            mpServiceProvider->playSound(data::SoundId::AlternateExplosion);
            shootableEntity.destroy();
          } else {
            mpServiceProvider->playSound(data::SoundId::EnemyHit);
          }
          break;
        }
      }
    });
}

}}
