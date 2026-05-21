#include "ASkySightGameModeBase.h"

#include "ASkySightPlayerController.h"

ASkySightGameModeBase::ASkySightGameModeBase()
{
	PlayerControllerClass = ASkySightPlayerController::StaticClass();
}
