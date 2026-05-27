from __future__ import annotations

from fire_uav.services.targets.target_tracker import (
    TargetObservation,
    TargetTrackState,
    TargetTracker,
)
from fire_uav.utils.time import utc_now


def test_tracker_moves_confirmed_target_to_in_orbit_and_orbited() -> None:
    tracker = TargetTracker(
        match_radius_m=30.0,
        suppression_radius_m=60.0,
        suppression_ttl_s=180.0,
        stable_frames_n=1,
    )
    now = utc_now()
    updates = tracker.update(
        [TargetObservation(class_label="1", lat=56.0, lon=92.9, timestamp=now, confidence=0.8)]
    )
    assert updates and updates[0].should_confirm is True
    track_id = updates[0].track.track_id
    assert tracker._tracks[track_id].state == TargetTrackState.CONFIRMED

    assert tracker.mark_in_orbit(track_id) is True
    assert tracker._tracks[track_id].state == TargetTrackState.IN_ORBIT

    assert tracker.mark_orbited(track_id) is True
    assert tracker._tracks[track_id].state == TargetTrackState.ORBITED


def test_tracker_merges_confirmed_fire_with_small_jitter_after_stabilization() -> None:
    tracker = TargetTracker(
        match_radius_m=30.0,
        suppression_radius_m=60.0,
        suppression_ttl_s=180.0,
        stable_frames_n=1,
    )
    now = utc_now()
    first = tracker.update(
        [TargetObservation(class_label="1", lat=47.606000, lon=-122.335000, timestamp=now, confidence=0.9)]
    )
    second = tracker.update(
        [TargetObservation(class_label="1", lat=47.606120, lon=-122.334960, timestamp=now, confidence=0.8)]
    )

    assert first and second
    assert second[0].track.track_id == first[0].track.track_id


def test_tracker_allows_new_confirmation_after_moderate_shift_outside_reduced_radius() -> None:
    tracker = TargetTracker(
        match_radius_m=35.0,
        suppression_radius_m=30.0,
        suppression_ttl_s=180.0,
        stable_frames_n=1,
    )
    now = utc_now()
    first = tracker.update(
        [TargetObservation(class_label="1", lat=47.606000, lon=-122.335000, timestamp=now, confidence=0.9)]
    )
    assert first and first[0].should_confirm is True

    track_id = first[0].track.track_id
    assert tracker.mark_orbited(track_id, now=now) is True

    second = tracker.update(
        [
            TargetObservation(
                class_label="1",
                lat=47.606360,
                lon=-122.334960,
                timestamp=now,
                confidence=0.8,
            )
        ]
    )

    assert second and second[0].should_confirm is True
    assert second[0].track.track_id != track_id
