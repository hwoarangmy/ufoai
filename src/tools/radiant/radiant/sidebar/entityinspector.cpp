/**
 * @file entityinspector.cpp
 */

/*
 Copyright (C) 1999-2006 Id Software, Inc. and contributors.
 For a list of contributors, see the accompanying CONTRIBUTORS file.

 This file is part of GtkRadiant.

 GtkRadiant is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 GtkRadiant is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with GtkRadiant; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "entityinspector.h"
#include "radiant_i18n.h"

#include "debugging/debugging.h"

#include "ientity.h"
#include "ifilesystem.h"
#include "imodel.h"
#include "iscenegraph.h"
#include "iselection.h"
#include "iundo.h"

#include <map>
#include <set>

#include "os/path.h"
#include "eclasslib.h"
#include "scenelib.h"
#include "generic/callback.h"
#include "os/file.h"
#include "stream/stringstream.h"
#include "moduleobserver.h"
#include "convert.h"
#include "stringio.h"

#include "gtkutil/accelerator.h"
#include "gtkutil/dialog.h"
#include "gtkutil/filechooser.h"
#include "gtkutil/messagebox.h"
#include "gtkutil/nonmodal.h"
#include "gtkutil/button.h"
#include "gtkutil/entry.h"
#include "gtkutil/container.h"

#include "../qe3.h"
#include "../gtkmisc.h"
#include "../entity.h"
#include "../mainframe.h"
#include "../textureentry.h"

static GtkEntry* numeric_entry_new (void)
{
	GtkEntry* entry = GTK_ENTRY(gtk_entry_new());
	gtk_widget_show(GTK_WIDGET(entry));
	widget_set_size(GTK_WIDGET(entry), 10, 0);
	return entry;
}

namespace
{
	typedef std::list<CopiedString> Values;
	typedef std::map<CopiedString, Values> KeyValues;
	typedef std::map<CopiedString, KeyValues> ClassKeyValues;
	ClassKeyValues g_selectedKeyValues; /**< all selected entities keyvalues */

	const EntityClass* g_current_attributes = 0;
	CopiedString g_currentSelectedKey;

}

/**
 * @brief Helper method returns value of the first list element
 * @param values value list to get first data element from
 * @return content of first list element
 */
const char* Values_getFirstValue(const Values &values)
{
	ASSERT_MESSAGE(!values.empty(), "Values don't exist");
	return (*values.begin()).c_str();
}

/**
 * @brief Helper method returns the first value in value list for given key for currently selected entity class or an empty string.
 * @param key key to retrieve value for
 * @return first value in value list or empty string
 */
const char* SelectedEntity_getValueForKey (const char* key)
{
	ASSERT_MESSAGE(g_current_attributes != 0, "g_current_attributes is zero");
	ClassKeyValues::iterator it = g_selectedKeyValues.find(g_current_attributes->m_name);
	if (it != g_selectedKeyValues.end()) {
		KeyValues &possibleValues = (*it).second;
		KeyValues::const_iterator i = possibleValues.find(key);
		if (i != possibleValues.end()) {
			return Values_getFirstValue((*i).second);
		}
	}
	return "";
}

static void Scene_EntitySetKeyValue_Selected_Undoable (const char* classname, const char* key, const char* value)
{
	StringOutputStream command(256);
	command << "entitySetKeyValue -classname " << makeQuoted(classname) << " -key " << makeQuoted(key) << " -value " << makeQuoted(value);
	UndoableCommand undo(command.c_str());
	Scene_EntitySetKeyValue_Selected(classname, key, value);
}

class EntityAttribute
{
	public:
		virtual ~EntityAttribute (void)
		{
		}
		virtual GtkWidget* getWidget () const = 0;
		virtual void update () = 0;
};

class BooleanAttribute: public EntityAttribute
{
		CopiedString m_classname;
		CopiedString m_key;
		GtkCheckButton* m_check;

		static gboolean toggled (GtkWidget *widget, BooleanAttribute* self)
		{
			self->apply();
			return FALSE;
		}
	public:
		BooleanAttribute (const char* classname, const char* key) :
			m_classname(classname), m_key(key), m_check(0)
		{
			GtkCheckButton* check = GTK_CHECK_BUTTON(gtk_check_button_new());
			gtk_widget_show(GTK_WIDGET(check));

			m_check = check;

			guint handler = g_signal_connect(G_OBJECT(check), "toggled", G_CALLBACK(toggled), this);
			g_object_set_data(G_OBJECT(check), "handler", gint_to_pointer(handler));

			update();
		}
		GtkWidget* getWidget () const
		{
			return GTK_WIDGET(m_check);
		}

		void apply (void)
		{
			Scene_EntitySetKeyValue_Selected_Undoable(m_classname.c_str(), m_key.c_str(), gtk_toggle_button_get_active(
					GTK_TOGGLE_BUTTON(m_check)) ? "1" : "0");
		}
		typedef MemberCaller<BooleanAttribute, &BooleanAttribute::apply> ApplyCaller;

		void update (void)
		{
			const char* value = SelectedEntity_getValueForKey(m_key.c_str());
			if (!string_empty(value)) {
				toggle_button_set_active_no_signal(GTK_TOGGLE_BUTTON(m_check), atoi(value) != 0);
			} else {
				toggle_button_set_active_no_signal(GTK_TOGGLE_BUTTON(m_check), false);
			}
		}
		typedef MemberCaller<BooleanAttribute, &BooleanAttribute::update> UpdateCaller;
};

class StringAttribute: public EntityAttribute
{
		CopiedString m_classname;
		CopiedString m_key;
		GtkEntry* m_entry;
		NonModalEntry m_nonModal;
	public:
		StringAttribute (const char* classname, const char* key) :
			m_classname(classname), m_key(key), m_entry(0), m_nonModal(ApplyCaller(*this), UpdateCaller(*this))
		{
			GtkEntry* entry = GTK_ENTRY(gtk_entry_new());
			gtk_widget_show(GTK_WIDGET(entry));
			widget_set_size(GTK_WIDGET(entry), 50, 0);

			m_entry = entry;
			m_nonModal.connect(m_entry);
		}
		GtkWidget* getWidget () const
		{
			return GTK_WIDGET(m_entry);
		}
		GtkEntry* getEntry () const
		{
			return m_entry;
		}

		void apply (void)
		{
			StringOutputStream value(64);
			value << ConvertUTF8ToLocale(gtk_entry_get_text(m_entry));
			Scene_EntitySetKeyValue_Selected_Undoable(m_classname.c_str(), m_key.c_str(), value.c_str());
		}
		typedef MemberCaller<StringAttribute, &StringAttribute::apply> ApplyCaller;

		void update (void)
		{
			StringOutputStream value(64);
			value << ConvertLocaleToUTF8(SelectedEntity_getValueForKey(m_key.c_str()));
			gtk_entry_set_text(m_entry, value.c_str());
		}
		typedef MemberCaller<StringAttribute, &StringAttribute::update> UpdateCaller;
};

class ModelAttribute: public EntityAttribute
{
		CopiedString m_classname;
		CopiedString m_key;
		BrowsedPathEntry m_entry;
		NonModalEntry m_nonModal;
	public:
		ModelAttribute (const char* classname, const char* key) :
			m_classname(classname), m_key(key), m_entry(BrowseCaller(*this)), m_nonModal(ApplyCaller(*this),
					UpdateCaller(*this))
		{
			m_nonModal.connect(m_entry.m_entry.m_entry);
		}

		GtkWidget* getWidget () const
		{
			return GTK_WIDGET(m_entry.m_entry.m_frame);
		}
		void apply (void)
		{
			StringOutputStream value(64);
			value << ConvertUTF8ToLocale(gtk_entry_get_text(GTK_ENTRY(m_entry.m_entry.m_entry)));
			Scene_EntitySetKeyValue_Selected_Undoable(m_classname.c_str(), m_key.c_str(), value.c_str());
		}
		typedef MemberCaller<ModelAttribute, &ModelAttribute::apply> ApplyCaller;
		void update (void)
		{
			StringOutputStream value(64);
			value << ConvertLocaleToUTF8(SelectedEntity_getValueForKey(m_key.c_str()));
			gtk_entry_set_text(GTK_ENTRY(m_entry.m_entry.m_entry), value.c_str());
		}
		typedef MemberCaller<ModelAttribute, &ModelAttribute::update> UpdateCaller;
		void browse (const BrowsedPathEntry::SetPathCallback& setPath)
		{
			const char *filename = misc_model_dialog(gtk_widget_get_toplevel(GTK_WIDGET(m_entry.m_entry.m_frame)));

			if (filename != 0) {
				setPath(filename);
				apply();
			}
		}
		typedef MemberCaller1<ModelAttribute, const BrowsedPathEntry::SetPathCallback&, &ModelAttribute::browse>
				BrowseCaller;
};

class SoundAttribute: public EntityAttribute
{
		CopiedString m_classname;
		CopiedString m_key;
		BrowsedPathEntry m_entry;
		NonModalEntry m_nonModal;
	public:
		SoundAttribute (const char* classname, const char* key) :
			m_classname(classname), m_key(key), m_entry(BrowseCaller(*this)), m_nonModal(ApplyCaller(*this),
					UpdateCaller(*this))
		{
			m_nonModal.connect(m_entry.m_entry.m_entry);
		}

		GtkWidget* getWidget () const
		{
			return GTK_WIDGET(m_entry.m_entry.m_frame);
		}
		void apply (void)
		{
			StringOutputStream value(64);
			value << ConvertUTF8ToLocale(gtk_entry_get_text(GTK_ENTRY(m_entry.m_entry.m_entry)));
			Scene_EntitySetKeyValue_Selected_Undoable(m_classname.c_str(), m_key.c_str(), value.c_str());
		}
		typedef MemberCaller<SoundAttribute, &SoundAttribute::apply> ApplyCaller;
		void update (void)
		{
			StringOutputStream value(64);
			value << ConvertLocaleToUTF8(SelectedEntity_getValueForKey(m_key.c_str()));
			gtk_entry_set_text(GTK_ENTRY(m_entry.m_entry.m_entry), value.c_str());
		}
		typedef MemberCaller<SoundAttribute, &SoundAttribute::update> UpdateCaller;
		void browse (const BrowsedPathEntry::SetPathCallback& setPath)
		{
			const char *filename = misc_sound_dialog(gtk_widget_get_toplevel(GTK_WIDGET(m_entry.m_entry.m_frame)));

			if (filename != 0) {
				setPath(filename);
				apply();
			}
		}
		typedef MemberCaller1<SoundAttribute, const BrowsedPathEntry::SetPathCallback&, &SoundAttribute::browse>
				BrowseCaller;
};

class ParticleAttribute: public EntityAttribute
{
		CopiedString m_classname;
		CopiedString m_key;
		BrowsedPathEntry m_entry;
		NonModalEntry m_nonModal;
	public:
		ParticleAttribute (const char* classname, const char* key) :
			m_classname(classname), m_key(key), m_entry(BrowseCaller(*this)), m_nonModal(ApplyCaller(*this),
					UpdateCaller(*this))
		{
			m_nonModal.connect(m_entry.m_entry.m_entry);
		}

		GtkWidget* getWidget () const
		{
			return GTK_WIDGET(m_entry.m_entry.m_frame);
		}
		void apply (void)
		{
			StringOutputStream value(64);
			value << ConvertUTF8ToLocale(gtk_entry_get_text(GTK_ENTRY(m_entry.m_entry.m_entry)));
			Scene_EntitySetKeyValue_Selected_Undoable(m_classname.c_str(), m_key.c_str(), value.c_str());
		}
		typedef MemberCaller<ParticleAttribute, &ParticleAttribute::apply> ApplyCaller;
		void update (void)
		{
			StringOutputStream value(64);
			value << ConvertLocaleToUTF8(SelectedEntity_getValueForKey(m_key.c_str()));
			gtk_entry_set_text(GTK_ENTRY(m_entry.m_entry.m_entry), value.c_str());
		}
		typedef MemberCaller<ParticleAttribute, &ParticleAttribute::update> UpdateCaller;
		void browse (const BrowsedPathEntry::SetPathCallback& setPath)
		{
			const char *filename = misc_particle_dialog(gtk_widget_get_toplevel(GTK_WIDGET(m_entry.m_entry.m_frame)));
			if (filename != 0) {
				setPath(filename);
				apply();
			}
		}
		typedef MemberCaller1<ParticleAttribute, const BrowsedPathEntry::SetPathCallback&, &ParticleAttribute::browse>
				BrowseCaller;
};

static inline double angle_normalised (double angle)
{
	return float_mod(angle, 360.0);
}

class AngleAttribute: public EntityAttribute
{
		CopiedString m_classname;
		CopiedString m_key;
		GtkEntry* m_entry;
		NonModalEntry m_nonModal;
	public:
		AngleAttribute (const char* classname, const char* key) :
			m_classname(classname), m_key(key), m_entry(0), m_nonModal(ApplyCaller(*this), UpdateCaller(*this))
		{
			GtkEntry* entry = numeric_entry_new();
			m_entry = entry;
			m_nonModal.connect(m_entry);
		}

		GtkWidget* getWidget () const
		{
			return GTK_WIDGET(m_entry);
		}
		void apply (void)
		{
			StringOutputStream angle(32);
			angle << angle_normalised(entry_get_float(m_entry));
			Scene_EntitySetKeyValue_Selected_Undoable(m_classname.c_str(), m_key.c_str(), angle.c_str());
		}
		typedef MemberCaller<AngleAttribute, &AngleAttribute::apply> ApplyCaller;

		void update (void)
		{
			const char* value = SelectedEntity_getValueForKey(m_key.c_str());
			if (!string_empty(value)) {
				StringOutputStream angle(32);
				angle << angle_normalised(atof(value));
				gtk_entry_set_text(m_entry, angle.c_str());
			} else {
				gtk_entry_set_text(m_entry, "0");
			}
		}
		typedef MemberCaller<AngleAttribute, &AngleAttribute::update> UpdateCaller;
};

static const char* directionButtons[] = { "up", "down", "z-axis" };

class DirectionAttribute: public EntityAttribute
{
		CopiedString m_classname;
		CopiedString m_key;
		GtkEntry* m_entry;
		NonModalEntry m_nonModal;
		RadioHBox m_radio;
		NonModalRadio m_nonModalRadio;
		GtkHBox* m_hbox;
	public:
		DirectionAttribute (const char* classname, const char* key) :
			m_classname(classname), m_key(key), m_entry(0), m_nonModal(ApplyCaller(*this), UpdateCaller(*this)),
					m_radio(RadioHBox_new(STRING_ARRAY_RANGE(directionButtons))), m_nonModalRadio(ApplyRadioCaller(
							*this))
		{
			GtkEntry* entry = numeric_entry_new();
			m_entry = entry;
			m_nonModal.connect(m_entry);

			m_nonModalRadio.connect(m_radio.m_radio);

			m_hbox = GTK_HBOX(gtk_hbox_new(FALSE, 4));
			gtk_widget_show(GTK_WIDGET(m_hbox));

			gtk_box_pack_start(GTK_BOX(m_hbox), GTK_WIDGET(m_radio.m_hbox), TRUE, TRUE, 0);
			gtk_box_pack_start(GTK_BOX(m_hbox), GTK_WIDGET(m_entry), TRUE, TRUE, 0);
		}

		GtkWidget* getWidget () const
		{
			return GTK_WIDGET(m_hbox);
		}
		void apply (void)
		{
			StringOutputStream angle(32);
			angle << angle_normalised(entry_get_float(m_entry));
			Scene_EntitySetKeyValue_Selected_Undoable(m_classname.c_str(), m_key.c_str(), angle.c_str());
		}
		typedef MemberCaller<DirectionAttribute, &DirectionAttribute::apply> ApplyCaller;

		void update (void)
		{
			const char* value = SelectedEntity_getValueForKey(m_key.c_str());
			if (!string_empty(value)) {
				const float f = float(atof(value));
				if (f == -1) {
					gtk_widget_set_sensitive(GTK_WIDGET(m_entry), FALSE);
					radio_button_set_active_no_signal(m_radio.m_radio, 0);
					gtk_entry_set_text(m_entry, "");
				} else if (f == -2) {
					gtk_widget_set_sensitive(GTK_WIDGET(m_entry), FALSE);
					radio_button_set_active_no_signal(m_radio.m_radio, 1);
					gtk_entry_set_text(m_entry, "");
				} else {
					gtk_widget_set_sensitive(GTK_WIDGET(m_entry), TRUE);
					radio_button_set_active_no_signal(m_radio.m_radio, 2);
					StringOutputStream angle(32);
					angle << angle_normalised(f);
					gtk_entry_set_text(m_entry, angle.c_str());
				}
			} else {
				gtk_entry_set_text(m_entry, "0");
			}
		}
		typedef MemberCaller<DirectionAttribute, &DirectionAttribute::update> UpdateCaller;

		void applyRadio (void)
		{
			const int index = radio_button_get_active(m_radio.m_radio);
			if (index == 0) {
				Scene_EntitySetKeyValue_Selected_Undoable(m_classname.c_str(), m_key.c_str(), "-1");
			} else if (index == 1) {
				Scene_EntitySetKeyValue_Selected_Undoable(m_classname.c_str(), m_key.c_str(), "-2");
			} else if (index == 2) {
				apply();
			}
		}
		typedef MemberCaller<DirectionAttribute, &DirectionAttribute::applyRadio> ApplyRadioCaller;
};

class AnglesEntry
{
	public:
		GtkEntry* m_roll;
		GtkEntry* m_pitch;
		GtkEntry* m_yaw;
		AnglesEntry () :
			m_roll(0), m_pitch(0), m_yaw(0)
		{
		}
};

typedef BasicVector3<double> DoubleVector3;

class AnglesAttribute: public EntityAttribute
{
		CopiedString m_classname;
		CopiedString m_key;
		AnglesEntry m_angles;
		NonModalEntry m_nonModal;
		GtkBox* m_hbox;
	public:
		AnglesAttribute (const char* classname, const char* key) :
			m_classname(classname), m_key(key), m_nonModal(ApplyCaller(*this), UpdateCaller(*this))
		{
			m_hbox = GTK_BOX(gtk_hbox_new(TRUE, 4));
			gtk_widget_show(GTK_WIDGET(m_hbox));
			{
				GtkEntry* entry = numeric_entry_new();
				gtk_box_pack_start(m_hbox, GTK_WIDGET(entry), TRUE, TRUE, 0);
				m_angles.m_pitch = entry;
				m_nonModal.connect(m_angles.m_pitch);
			}
			{
				GtkEntry* entry = numeric_entry_new();
				gtk_box_pack_start(m_hbox, GTK_WIDGET(entry), TRUE, TRUE, 0);
				m_angles.m_yaw = entry;
				m_nonModal.connect(m_angles.m_yaw);
			}
			{
				GtkEntry* entry = numeric_entry_new();
				gtk_box_pack_start(m_hbox, GTK_WIDGET(entry), TRUE, TRUE, 0);
				m_angles.m_roll = entry;
				m_nonModal.connect(m_angles.m_roll);
			}
		}

		GtkWidget* getWidget () const
		{
			return GTK_WIDGET(m_hbox);
		}
		void apply (void)
		{
			StringOutputStream angles(64);
			angles << angle_normalised(entry_get_float(m_angles.m_pitch)) << " " << angle_normalised(entry_get_float(
					m_angles.m_yaw)) << " " << angle_normalised(entry_get_float(m_angles.m_roll));
			Scene_EntitySetKeyValue_Selected_Undoable(m_classname.c_str(), m_key.c_str(), angles.c_str());
		}
		typedef MemberCaller<AnglesAttribute, &AnglesAttribute::apply> ApplyCaller;

		void update (void)
		{
			StringOutputStream angle(32);
			const char* value = SelectedEntity_getValueForKey(m_key.c_str());
			if (!string_empty(value)) {
				DoubleVector3 pitch_yaw_roll;
				if (!string_parse_vector3(value, pitch_yaw_roll)) {
					pitch_yaw_roll = DoubleVector3(0, 0, 0);
				}

				angle << angle_normalised(pitch_yaw_roll.x());
				gtk_entry_set_text(m_angles.m_pitch, angle.c_str());
				angle.clear();

				angle << angle_normalised(pitch_yaw_roll.y());
				gtk_entry_set_text(m_angles.m_yaw, angle.c_str());
				angle.clear();

				angle << angle_normalised(pitch_yaw_roll.z());
				gtk_entry_set_text(m_angles.m_roll, angle.c_str());
				angle.clear();
			} else {
				gtk_entry_set_text(m_angles.m_pitch, "0");
				gtk_entry_set_text(m_angles.m_yaw, "0");
				gtk_entry_set_text(m_angles.m_roll, "0");
			}
		}
		typedef MemberCaller<AnglesAttribute, &AnglesAttribute::update> UpdateCaller;
};

class Vector3Entry
{
	public:
		GtkEntry* m_x;
		GtkEntry* m_y;
		GtkEntry* m_z;
		Vector3Entry () :
			m_x(0), m_y(0), m_z(0)
		{
		}
};

class Vector3Attribute: public EntityAttribute
{
		CopiedString m_classname;
		CopiedString m_key;
		Vector3Entry m_vector3;
		NonModalEntry m_nonModal;
		GtkBox* m_hbox;
	public:
		Vector3Attribute (const char* classname, const char* key) :
			m_classname(classname), m_key(key), m_nonModal(ApplyCaller(*this), UpdateCaller(*this))
		{
			m_hbox = GTK_BOX(gtk_hbox_new(TRUE, 4));
			gtk_widget_show(GTK_WIDGET(m_hbox));
			{
				GtkEntry* entry = numeric_entry_new();
				gtk_box_pack_start(m_hbox, GTK_WIDGET(entry), TRUE, TRUE, 0);
				m_vector3.m_x = entry;
				m_nonModal.connect(m_vector3.m_x);
			}
			{
				GtkEntry* entry = numeric_entry_new();
				gtk_box_pack_start(m_hbox, GTK_WIDGET(entry), TRUE, TRUE, 0);
				m_vector3.m_y = entry;
				m_nonModal.connect(m_vector3.m_y);
			}
			{
				GtkEntry* entry = numeric_entry_new();
				gtk_box_pack_start(m_hbox, GTK_WIDGET(entry), TRUE, TRUE, 0);
				m_vector3.m_z = entry;
				m_nonModal.connect(m_vector3.m_z);
			}
		}

		GtkWidget* getWidget () const
		{
			return GTK_WIDGET(m_hbox);
		}
		void apply (void)
		{
			StringOutputStream vector3(64);
			vector3 << entry_get_float(m_vector3.m_x) << " " << entry_get_float(m_vector3.m_y) << " "
					<< entry_get_float(m_vector3.m_z);
			Scene_EntitySetKeyValue_Selected_Undoable(m_classname.c_str(), m_key.c_str(), vector3.c_str());
		}
		typedef MemberCaller<Vector3Attribute, &Vector3Attribute::apply> ApplyCaller;

		void update (void)
		{
			StringOutputStream buffer(32);
			const char* value = SelectedEntity_getValueForKey(m_key.c_str());
			if (!string_empty(value)) {
				DoubleVector3 x_y_z;
				if (!string_parse_vector3(value, x_y_z)) {
					x_y_z = DoubleVector3(0, 0, 0);
				}

				buffer << x_y_z.x();
				gtk_entry_set_text(m_vector3.m_x, buffer.c_str());
				buffer.clear();

				buffer << x_y_z.y();
				gtk_entry_set_text(m_vector3.m_y, buffer.c_str());
				buffer.clear();

				buffer << x_y_z.z();
				gtk_entry_set_text(m_vector3.m_z, buffer.c_str());
				buffer.clear();
			} else {
				gtk_entry_set_text(m_vector3.m_x, "0");
				gtk_entry_set_text(m_vector3.m_y, "0");
				gtk_entry_set_text(m_vector3.m_z, "0");
			}
		}
		typedef MemberCaller<Vector3Attribute, &Vector3Attribute::update> UpdateCaller;
};

class NonModalComboBox
{
		Callback m_changed;
		guint m_changedHandler;

		static gboolean changed (GtkComboBox *widget, NonModalComboBox* self)
		{
			self->m_changed();
			return FALSE;
		}

	public:
		NonModalComboBox (const Callback& changed) :
			m_changed(changed), m_changedHandler(0)
		{
		}
		void connect (GtkComboBox* combo)
		{
			m_changedHandler = g_signal_connect(G_OBJECT(combo), "changed", G_CALLBACK(changed), this);
		}
		void setActive (GtkComboBox* combo, int value)
		{
			g_signal_handler_disconnect(G_OBJECT(combo), m_changedHandler);
			gtk_combo_box_set_active(combo, value);
			connect(combo);
		}
};

class ListAttribute: public EntityAttribute
{
		CopiedString m_classname;
		CopiedString m_key;
		GtkComboBox* m_combo;
		NonModalComboBox m_nonModal;
		const ListAttributeType& m_type;
	public:
		ListAttribute (const char* classname, const char* key, const ListAttributeType& type) :
			m_classname(classname), m_key(key), m_combo(0), m_nonModal(ApplyCaller(*this)), m_type(type)
		{
			GtkComboBox* combo = GTK_COMBO_BOX(gtk_combo_box_new_text());

			for (ListAttributeType::const_iterator i = type.begin(); i != type.end(); ++i) {
				gtk_combo_box_append_text(GTK_COMBO_BOX(combo), (*i).first.c_str());
			}

			gtk_widget_show(GTK_WIDGET(combo));
			m_nonModal.connect(combo);

			m_combo = combo;
		}

		GtkWidget* getWidget () const
		{
			return GTK_WIDGET(m_combo);
		}
		void apply (void)
		{
			Scene_EntitySetKeyValue_Selected_Undoable(m_classname.c_str(), m_key.c_str(),
					m_type[gtk_combo_box_get_active(m_combo)].second.c_str());
		}
		typedef MemberCaller<ListAttribute, &ListAttribute::apply> ApplyCaller;

		void update (void)
		{
			const char* value = SelectedEntity_getValueForKey(m_key.c_str());
			ListAttributeType::const_iterator i = m_type.findValue(value);
			if (i != m_type.end()) {
				m_nonModal.setActive(m_combo, static_cast<int> (std::distance(m_type.begin(), i)));
			} else {
				m_nonModal.setActive(m_combo, 0);
			}
		}
		typedef MemberCaller<ListAttribute, &ListAttribute::update> UpdateCaller;
};

namespace
{
	enum {
		KEYVALLIST_COLUMN_KEY,
		KEYVALLIST_COLUMN_VALUE,
		KEYVALLIST_COLUMN_STYLE, /**< pango weigth value used to bold classname rows */
		KEYVALLIST_COLUMN_KEY_EDITABLE, /**< flag indicating whether key should be editable (has no value yet) */

		KEYVALLIST_MAX_COLUMN
	};

	/**
	 * @brief Container class holds references to different gui elements in Entity Inspector
	 */
	class EntityInspectorGuiElements
	{
		public:
			GtkButton *m_btnRemoveKey;
			GtkButton *m_btnAddKey;
			GtkTreeView *m_viewKeyValues;
	};

	static EntityInspectorGuiElements g_entityInspectorGui;

	GtkTreeView* g_entityClassList;
	GtkTextView* g_entityClassComment;

	GtkCheckButton* g_entitySpawnflagsCheck[MAX_FLAGS];

	GtkListStore* g_entlist_store;
	GtkTreeStore* g_entprops_store;
	const EntityClass* g_current_flags = 0;
	const EntityClass* g_current_comment = 0;

	// the number of active spawnflags
	int g_spawnflag_count;
	// table: index, match spawnflag item to the spawnflag index (i.e. which bit)
	int spawn_table[MAX_FLAGS];
	// we change the layout depending on how many spawn flags we need to display
	// the table is a 4x4 in which we need to put the comment box g_entityClassComment and the spawn flags..
	GtkTable* g_spawnflagsTable;

	GtkVBox* g_attributeBox = 0;
	typedef std::vector<EntityAttribute*> EntityAttributes;
	EntityAttributes g_entityAttributes;

	int g_numNewKeys = 0;
}

static void GlobalEntityAttributes_clear (void)
{
	for (EntityAttributes::iterator i = g_entityAttributes.begin(); i != g_entityAttributes.end(); ++i) {
		delete *i;
	}
	g_entityAttributes.clear();
}

/**
 * @brief visitor implementation used to add entities key/value pairs into a list.
 * @note key "classname" is ignored as this is supposed to be in parent map
 */
class GetKeyValueVisitor: public Entity::Visitor
{
		KeyValues &m_keyvalues;
	public:
		GetKeyValueVisitor (KeyValues& keyvalues):
			m_keyvalues(keyvalues)
		{
		}

		void visit (const char* key, const char* value)
		{
			/* don't add classname property, as this will be in parent list */
			if (!strcmp(key, "classname"))
				return;
			KeyValues::iterator keyIter = m_keyvalues.find(key);
			if (keyIter == m_keyvalues.end()) {
				m_keyvalues.insert(KeyValues::value_type(CopiedString(key),Values()));
				keyIter = m_keyvalues.find(key);
			}
			(*keyIter).second.push_back(Values::value_type(CopiedString(value)));
		}

};

/**
 * @brief Adds all entities class attributes into the given map.
 *
 * @param entity entity to add into map
 * @param keyvalues list of key/values associated to their class names
 */
void Entity_GetKeyValues (const Entity& entity, ClassKeyValues& keyvalues)
{
	CopiedString classname = entity.getEntityClass().m_name;
	ClassKeyValues::iterator valuesIter = keyvalues.find(classname);
	if (valuesIter == keyvalues.end()) {
		keyvalues.insert(ClassKeyValues::value_type(classname, KeyValues()));
		valuesIter = keyvalues.find(classname);
	}

	GetKeyValueVisitor visitor((*valuesIter).second);

	entity.forEachKeyValue(visitor);
}

/**
 * @brief Adds all currently selected entities attribute values to the given map.
 *
 * @param keyvalues map of entitites key value pairs associated to their classnames
 */
void Entity_GetKeyValues_Selected (ClassKeyValues& keyvalues)
{
	/**
	 * @brief visitor implementation used to add all selected entities key/value pairs
	 */
	class EntityGetKeyValues: public SelectionSystem::Visitor
	{
			ClassKeyValues& m_keyvalues;
			mutable std::set<Entity*> m_visited;
		public:
			EntityGetKeyValues (ClassKeyValues& keyvalues) :
				m_keyvalues(keyvalues)
			{
			}
			void visit (scene::Instance& instance) const
			{
				Entity* entity = Node_getEntity(instance.path().top());
				if (entity == 0 && instance.path().size() != 1) {
					entity = Node_getEntity(instance.path().parent());
				}
				if (entity != 0 && m_visited.insert(entity).second) {
					Entity_GetKeyValues(*entity, m_keyvalues);
				}
			}
	} visitor(keyvalues);
	GlobalSelectionSystem().foreachSelected(visitor);
}

class EntityClassListStoreAppend: public EntityClassVisitor
{
		GtkListStore* store;
	public:
		EntityClassListStoreAppend (GtkListStore* store_) :
			store(store_)
		{
		}
		void visit (EntityClass* e)
		{
			GtkTreeIter iter;
			gtk_list_store_append(store, &iter);
			gtk_list_store_set(store, &iter, 0, e->name(), 1, e, -1);
		}
};

static void EntityClassList_fill (void)
{
	if (g_entlist_store) {
		EntityClassListStoreAppend append(g_entlist_store);
		GlobalEntityClassManager().forEach(append);
	}
}

static void EntityClassList_clear (void)
{
	if (g_entlist_store) {
		gtk_list_store_clear(g_entlist_store);
	}
}

static void SetComment (EntityClass* eclass)
{
	if (eclass == g_current_comment)
		return;

	g_current_comment = eclass;

	GtkTextBuffer* buffer = gtk_text_view_get_buffer(g_entityClassComment);
	gtk_text_buffer_set_text(buffer, eclass->comments(), -1);
}

/**
 * @brief Updates surface flags buttons to display given entity class spawnflags
 * @param eclass entity class to display
 * @note This method hides all buttons, then updates as much as needed with new label and shows them.
 * @sa EntityInspector_updateSpawnflags
 */
static void SurfaceFlags_setEntityClass (EntityClass* eclass)
{
	if (eclass == g_current_flags)
		return;

	g_current_flags = eclass;

	int spawnflag_count = 0;

	{
		// do a first pass to count the spawn flags, don't touch the widgets, we don't know in what state they are
		for (int i = 0; i < MAX_FLAGS; i++) {
			if (eclass->flagnames[i] && eclass->flagnames[i][0] != 0 && strcmp(eclass->flagnames[i], "-")) {
				spawn_table[spawnflag_count] = i;
				spawnflag_count++;
			}
		}
	}

	// disable all remaining boxes
	// NOTE: these boxes might not even be on display
	{
		for (int i = 0; i < g_spawnflag_count; ++i) {
			GtkWidget* widget = GTK_WIDGET(g_entitySpawnflagsCheck[i]);
			gtk_label_set_text(GTK_LABEL(GTK_BIN(widget)->child), " ");
			gtk_widget_hide(widget);
			gtk_widget_ref(widget);
			gtk_container_remove(GTK_CONTAINER(g_spawnflagsTable), widget);
		}
	}

	g_spawnflag_count = spawnflag_count;

	{
		for (int i = 0; i < g_spawnflag_count; ++i) {
			GtkWidget* widget = GTK_WIDGET(g_entitySpawnflagsCheck[i]);
			gtk_widget_show(widget);

			StringOutputStream str(16);
			str << LowerCase(eclass->flagnames[spawn_table[i]]);

			gtk_table_attach(g_spawnflagsTable, widget, i % 4, i % 4 + 1, i / 4, i / 4 + 1,
					(GtkAttachOptions)(GTK_FILL), (GtkAttachOptions)(GTK_FILL), 0, 0);
			gtk_widget_unref(widget);

			gtk_label_set_text(GTK_LABEL(GTK_BIN(widget)->child), str.c_str());
		}
	}
}

static void EntityClassList_selectEntityClass (EntityClass* eclass)
{
	GtkTreeModel* model = GTK_TREE_MODEL(g_entlist_store);
	GtkTreeIter iter;
	for (gboolean good = gtk_tree_model_get_iter_first(model, &iter); good != FALSE; good = gtk_tree_model_iter_next(
			model, &iter)) {
		char* text;
		gtk_tree_model_get(model, &iter, 0, &text, -1);
		if (!strcmp(text, eclass->name())) {
			GtkTreeView* view = g_entityClassList;
			GtkTreePath* path = gtk_tree_model_get_path(model, &iter);
			gtk_tree_selection_select_path(gtk_tree_view_get_selection(view), path);
			if (GTK_WIDGET_REALIZED(view)) {
				gtk_tree_view_scroll_to_cell(view, path, 0, FALSE, 0, 0);
			}
			gtk_tree_path_free(path);
			good = FALSE;
		}
		g_free(text);
	}
}

static void EntityInspector_appendAttribute (const char* name, EntityAttribute& attribute)
{
	GtkTable* row = DialogRow_new(name, attribute.getWidget());
	DialogVBox_packRow(g_attributeBox, GTK_WIDGET(row));
}

template<typename Attribute>
class StatelessAttributeCreator
{
	public:
		static EntityAttribute* create (const char* classname, const char* name)
		{
			return new Attribute(classname, name);
		}
};

class EntityAttributeFactory
{
		typedef EntityAttribute* (*CreateFunc) (const char* classname, const char* name);
		typedef std::map<const char*, CreateFunc, RawStringLess> Creators;
		Creators m_creators;
	public:
		EntityAttributeFactory (void)
		{
			m_creators.insert(Creators::value_type("string", &StatelessAttributeCreator<StringAttribute>::create));
			m_creators.insert(Creators::value_type("color", &StatelessAttributeCreator<StringAttribute>::create));
			m_creators.insert(Creators::value_type("integer", &StatelessAttributeCreator<StringAttribute>::create));
			m_creators.insert(Creators::value_type("real", &StatelessAttributeCreator<StringAttribute>::create));
			m_creators.insert(Creators::value_type("boolean", &StatelessAttributeCreator<BooleanAttribute>::create));
			m_creators.insert(Creators::value_type("angle", &StatelessAttributeCreator<AngleAttribute>::create));
			m_creators.insert(Creators::value_type("direction", &StatelessAttributeCreator<DirectionAttribute>::create));
			m_creators.insert(Creators::value_type("angles", &StatelessAttributeCreator<AnglesAttribute>::create));
			m_creators.insert(Creators::value_type("particle", &StatelessAttributeCreator<ParticleAttribute>::create));
			m_creators.insert(Creators::value_type("model", &StatelessAttributeCreator<ModelAttribute>::create));
			m_creators.insert(Creators::value_type("noise", &StatelessAttributeCreator<SoundAttribute>::create));
			m_creators.insert(Creators::value_type("vector3", &StatelessAttributeCreator<Vector3Attribute>::create));
		}
		EntityAttribute* create (const char* type, const char* name)
		{
			const char* classname = g_current_attributes->name();
			Creators::iterator i = m_creators.find(type);
			if (i != m_creators.end()) {
				return (*i).second(classname, name);
			}
			const ListAttributeType* listType = GlobalEntityClassManager().findListType(type);
			if (listType != 0) {
				return new ListAttribute(classname, name, *listType);
			}
			return 0;
		}
};

typedef Static<EntityAttributeFactory> GlobalEntityAttributeFactory;

static void EntityInspector_checkAddNewKeys (void)
{
	int count = 0;
	const EntityClassAttributes validAttrib = g_current_attributes->m_attributes;
	for (EntityClassAttributes::const_iterator i = validAttrib.begin(); i != validAttrib.end(); ++i) {
		EntityClassAttribute *attrib = const_cast<EntityClassAttribute*> (&(*i).second);
		ClassKeyValues::iterator it = g_selectedKeyValues.find(g_current_attributes->m_name);
		if (it == g_selectedKeyValues.end()) {
			return;
		}
		KeyValues possibleValues = (*it).second;
		KeyValues::const_iterator keyIter = possibleValues.find(attrib->m_type.c_str());
		/* end means we don't have it actually in this map, so this is a valid new key*/
		if (keyIter == possibleValues.end()) {
			count++;
		}
	}
	g_numNewKeys = count;
}

static void EntityInspector_setEntityClass (EntityClass *eclass)
{
	EntityClassList_selectEntityClass(eclass);
	SurfaceFlags_setEntityClass(eclass);

	if (eclass != g_current_attributes) {
		g_current_attributes = eclass;

		container_remove_all(GTK_CONTAINER(g_attributeBox));
		GlobalEntityAttributes_clear();

		for (EntityClassAttributes::const_iterator i = eclass->m_attributes.begin(); i != eclass->m_attributes.end(); ++i) {
			EntityAttribute* attribute = GlobalEntityAttributeFactory::instance().create((*i).second.m_type.c_str(),
					(*i).first.c_str());
			if (attribute != 0) {
				g_entityAttributes.push_back(attribute);
				EntityInspector_appendAttribute(EntityClassAttributePair_getName(*i), *g_entityAttributes.back());
			}
		}
	}
}

/**
 * @brief This method updates active state of spawnflag buttons based on current selected value
 * @sa SurfaceFlags_setEntityClass
 * @sa EntityInspector_updateGuiElements
 */
static void EntityInspector_updateSpawnflags (void)
{
	int i;
	const int f = atoi(SelectedEntity_getValueForKey("spawnflags"));
	for (i = 0; i < g_spawnflag_count; ++i) {
		const int v = !!(f & (1 << spawn_table[i]));

		toggle_button_set_active_no_signal(GTK_TOGGLE_BUTTON(g_entitySpawnflagsCheck[i]), v);
	}

	// take care of the remaining ones
	for (i = g_spawnflag_count; i < MAX_FLAGS; ++i) {
		toggle_button_set_active_no_signal(GTK_TOGGLE_BUTTON(g_entitySpawnflagsCheck[i]), FALSE);
	}
}

/**
 * @brief This method updates key value list for current selected entities.
 * @sa EntityInspector_updateGuiElements
 */
static void EntityInspector_updateKeyValueList (void)
{
	GtkTreeStore* store = g_entprops_store;

	gtk_tree_store_clear(store);
	// Walk through list and add pairs
	for (ClassKeyValues::iterator classIter = g_selectedKeyValues.begin(); classIter != g_selectedKeyValues.end(); ++classIter) {
		CopiedString classname((*classIter).first.c_str());
		GtkTreeIter classTreeIter;
		gtk_tree_store_append(store, &classTreeIter, NULL);
		gtk_tree_store_set(store, &classTreeIter, KEYVALLIST_COLUMN_KEY, "classname", KEYVALLIST_COLUMN_VALUE,
				classname.c_str(), KEYVALLIST_COLUMN_STYLE, PANGO_WEIGHT_BOLD, KEYVALLIST_COLUMN_KEY_EDITABLE, FALSE, -1);
		KeyValues possibleValues = classIter->second;
		for (KeyValues::iterator i = possibleValues.begin(); i != possibleValues.end(); ++i) {
			GtkTreeIter iter;
			gtk_tree_store_append(store, &iter, &classTreeIter);
			StringOutputStream key(64);
			key << ConvertLocaleToUTF8((*i).first.c_str());
			StringOutputStream value(64);
			value << ConvertLocaleToUTF8(Values_getFirstValue((*i).second));
			gtk_tree_store_set(store, &iter, KEYVALLIST_COLUMN_KEY, key.c_str(), KEYVALLIST_COLUMN_VALUE,
					value.c_str(), KEYVALLIST_COLUMN_STYLE, PANGO_WEIGHT_NORMAL, KEYVALLIST_COLUMN_KEY_EDITABLE, FALSE,
					-1);
		}
	}
	//set all elements expanded
	gtk_tree_view_expand_all(g_entityInspectorGui.m_viewKeyValues);
	EntityInspector_checkAddNewKeys();
}

void EntityInspector_updateGuiElements (void)
{
	g_selectedKeyValues.clear();
	Entity_GetKeyValues_Selected(g_selectedKeyValues);

	{
		const char* classname = "";
		ClassKeyValues::iterator classFirstIter = g_selectedKeyValues.begin();
		if (classFirstIter != g_selectedKeyValues.end()) {
			classname = const_cast<char *> ((*classFirstIter).first.c_str());
		}
		EntityInspector_setEntityClass(GlobalEntityClassManager().findOrInsert(classname, false));
	}
	EntityInspector_updateSpawnflags();

	EntityInspector_updateKeyValueList();

	for (EntityAttributes::const_iterator i = g_entityAttributes.begin(); i != g_entityAttributes.end(); ++i) {
		(*i)->update();
	}
}

class EntityInspectorDraw
{
		IdleDraw m_idleDraw;
	public:
		EntityInspectorDraw () :
			m_idleDraw(FreeCaller<EntityInspector_updateGuiElements> ())
		{
		}
		void queueDraw (void)
		{
			m_idleDraw.queueDraw();
		}
};

static EntityInspectorDraw g_EntityInspectorDraw;

void EntityInspector_keyValueChanged (void)
{
	g_EntityInspectorDraw.queueDraw();
}

void EntityInspector_selectionChanged (const Selectable&)
{
	EntityInspector_keyValueChanged();
}

// Creates a new entity based on the currently selected brush and entity type.
//
static void EntityClassList_createEntity (void)
{
	GtkTreeView* view = g_entityClassList;

	// find out what type of entity we are trying to create
	GtkTreeModel* model;
	GtkTreeIter iter;
	if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(view), &model, &iter) == FALSE) {
		gtk_MessageBox(gtk_widget_get_toplevel(GTK_WIDGET(g_entityClassList)),
				_("You must have a selected class to create an entity"), _("Info"));
		return;
	}

	char* text;
	gtk_tree_model_get(model, &iter, 0, &text, -1);

	{
		StringOutputStream command;
		command << "entityCreate -class " << text;

		UndoableCommand undo(command.c_str());

		Entity_createFromSelection(text, g_vector3_identity);
	}
	g_free(text);
}

static void EntityInspector_addKeyValue (GtkButton *button, gpointer user_data)
{
	GtkTreeIter iter;
	GtkTreeIter parent;
	GtkTreeView *view = GTK_TREE_VIEW(user_data);
	GtkTreeModel* model = gtk_tree_view_get_model(view);
	GtkTreeStore *store = GTK_TREE_STORE(model);
	GtkTreeSelection *selection = gtk_tree_view_get_selection(view);

	gtk_tree_selection_get_selected(selection, &model, &iter);
	if (gtk_tree_model_iter_parent(model, &parent, &iter) == FALSE)
	{
		// parent node selected, just add child
		parent = iter;
		gtk_tree_store_append(store, &iter, &parent);
	} else {
		gtk_tree_store_append(store, &iter, &parent);
	}

	gtk_tree_store_set(store, &iter, KEYVALLIST_COLUMN_KEY, "", KEYVALLIST_COLUMN_VALUE, "", KEYVALLIST_COLUMN_STYLE, PANGO_WEIGHT_NORMAL, KEYVALLIST_COLUMN_KEY_EDITABLE, TRUE, -1);
	/* expand to have added line visible (for the case parent was not yet expanded because it had no children */
	gtk_tree_view_expand_all(g_entityInspectorGui.m_viewKeyValues);

	/* select newly added field and start editing */
	GtkTreeViewColumn* viewColumn = gtk_tree_view_get_column(view, 0);
	GtkTreePath* path = gtk_tree_model_get_path(model, &iter);
	gtk_tree_view_set_cursor(view, path, viewColumn, TRUE);
	gtk_tree_path_free(path);
	//disable new key as long as we edit
	g_object_set(g_entityInspectorGui.m_btnAddKey, "sensitive", FALSE, (const char*)0);
}

static void EntityInspector_clearKeyValue (GtkButton * button, gpointer user_data)
{
	GtkTreeModel* model;
	GtkTreeView *view = GTK_TREE_VIEW(user_data);
	GtkTreeIter iter, parent;
	GtkTreeSelection* selection = gtk_tree_view_get_selection(view);
	if (gtk_tree_selection_get_selected(selection, &model, &iter) == FALSE) {
		return;
	}

	char* key, *classname;
	gtk_tree_model_get(model, &iter, KEYVALLIST_COLUMN_KEY, &key, -1);
	if (gtk_tree_model_iter_parent(model, &parent, &iter) == FALSE) {
		gtk_tree_model_get(model, &iter, KEYVALLIST_COLUMN_VALUE, &classname, -1);
	} else {
		gtk_tree_model_get(model, &parent, KEYVALLIST_COLUMN_VALUE, &classname, -1);
	}

	// Get current selection text
	StringOutputStream keyConverted(64);
	keyConverted << ConvertUTF8ToLocale(key);
	StringOutputStream classnameConverted(64);
	classnameConverted << ConvertUTF8ToLocale(classname);

	if (strcmp(keyConverted.c_str(), "classname") != 0) {
		StringOutputStream command;
		command << "entityDeleteKey -classname " << classnameConverted.c_str() << " -key " << keyConverted.c_str();
		UndoableCommand undo(command.c_str());
		Scene_EntitySetKeyValue_Selected(classnameConverted.c_str(), keyConverted.c_str(), "");
	}
}

// =============================================================================
// callbacks

static void EntityClassList_selection_changed (GtkTreeSelection* selection, gpointer data)
{
	GtkTreeModel* model;
	GtkTreeIter selected;
	if (gtk_tree_selection_get_selected(selection, &model, &selected)) {
		EntityClass* eclass;
		gtk_tree_model_get(model, &selected, 1, &eclass, -1);
		if (eclass != 0) {
			SetComment(eclass);
		}
	}
}

static gint EntityClassList_button_press (GtkWidget *widget, GdkEventButton *event, gpointer data)
{
	if (event->type == GDK_2BUTTON_PRESS) {
		EntityClassList_createEntity();
		return TRUE;
	}
	return FALSE;
}

static gint EntityClassList_keypress (GtkWidget* widget, GdkEventKey* event, gpointer data)
{
	unsigned int code = gdk_keyval_to_upper(event->keyval);

	if (event->keyval == GDK_Return) {
		EntityClassList_createEntity();
		return TRUE;
	}

	// select the entity that starts with the key pressed
	if (code <= 'Z' && code >= 'A') {
		GtkTreeView* view = g_entityClassList;
		GtkTreeModel* model;
		GtkTreeIter iter;
		if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(view), &model, &iter) == FALSE
				|| gtk_tree_model_iter_next(model, &iter) == FALSE) {
			gtk_tree_model_get_iter_first(model, &iter);
		}

		for (std::size_t count = gtk_tree_model_iter_n_children(model, 0); count > 0; --count) {
			char* text;
			gtk_tree_model_get(model, &iter, 0, &text, -1);

			if (toupper(text[0]) == (int) code) {
				GtkTreePath* path = gtk_tree_model_get_path(model, &iter);
				gtk_tree_selection_select_path(gtk_tree_view_get_selection(view), path);
				if (GTK_WIDGET_REALIZED(view)) {
					gtk_tree_view_scroll_to_cell(view, path, 0, FALSE, 0, 0);
				}
				gtk_tree_path_free(path);
				count = 1;
			}

			g_free(text);

			if (gtk_tree_model_iter_next(model, &iter) == FALSE)
				gtk_tree_model_get_iter_first(model, &iter);
		}

		return TRUE;
	}
	return FALSE;
}

static void SpawnflagCheck_toggled (GtkWidget *widget, gpointer data)
{
	int f, i;
	char sz[32];

	f = 0;
	for (i = 0; i < g_spawnflag_count; ++i) {
		const int v = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_entitySpawnflagsCheck[i]));
		f |= v << spawn_table[i];
	}

	sprintf(sz, "%i", f);
	const char* value = (f == 0) ? "" : sz;

	{
		StringOutputStream command;
		command << "entitySetFlags -classname " << g_current_flags->name() << "-flags " << f;
		UndoableCommand undo(command.c_str());

		Scene_EntitySetKeyValue_Selected(g_current_flags->name(), "spawnflags", value);
	}
}

static void entityKeyValueEdited (GtkTreeView *view, int columnIndex, char *newValue)
{
	char *key, *value, *classname;
	bool isClassname = false;
	GtkTreeModel* model;
	GtkTreeIter iter, parent;
	GtkTreeSelection* selection = gtk_tree_view_get_selection(view);

	if (gtk_tree_selection_get_selected(selection, &model, &iter) == FALSE) {
		g_warning("No entity parameter selected to change the value for\n");
		return;
	}

	if (columnIndex == 1) {
		gtk_tree_model_get(model, &iter, KEYVALLIST_COLUMN_KEY, &key, -1);
		value = newValue;
	} else {
		gtk_tree_model_get(model, &iter, KEYVALLIST_COLUMN_VALUE, &value, -1);
		key = newValue;
	}
	if (gtk_tree_model_iter_parent(model, &parent, &iter) == FALSE) {
		gtk_tree_model_get(model, &iter, KEYVALLIST_COLUMN_VALUE, &classname, -1);
		isClassname = true;
	} else {
		gtk_tree_model_get(model, &parent, KEYVALLIST_COLUMN_VALUE, &classname, -1);
	}

	// Get current selection text
	StringOutputStream keyConverted(64);
	keyConverted << ConvertUTF8ToLocale(key);
	StringOutputStream valueConverted(64);
	valueConverted << ConvertUTF8ToLocale(value);
	StringOutputStream classnameConverted(64);
	classnameConverted << ConvertUTF8ToLocale(classname);

	// if you change the classname to worldspawn you won't merge back in the structural
	// brushes but create a parasite entity
	if (isClassname && !strcmp(valueConverted.c_str(), "worldspawn")) {
		gtk_MessageBox(0, _("Cannot change \"classname\" key back to worldspawn."), 0, eMB_OK);
		return;
	}

	// we don't want spaces in entity keys
	if (strstr(keyConverted.c_str(), " ")) {
		gtk_MessageBox(0, _("No spaces are allowed in entity keys."), 0, eMB_OK);
		return;
	}

	if (columnIndex == 0 && !keyConverted.empty() && valueConverted.empty())
		valueConverted << g_current_attributes->getDefaultForAttribute(keyConverted.c_str());

	g_message("change value for %s to %s\n", keyConverted.c_str(), valueConverted.c_str());
	if (isClassname) {
		StringOutputStream command;
		command << "entitySetClass -class " << classnameConverted.c_str() << " -newclass " << valueConverted.c_str();
		UndoableCommand undo(command.c_str());
		Scene_EntitySetClassname_Selected(classnameConverted.c_str(), valueConverted.c_str());
	} else {
		Scene_EntitySetKeyValue_Selected_Undoable(classnameConverted.c_str(), keyConverted.c_str(), valueConverted.c_str());
	}
	gtk_tree_store_remove(GTK_TREE_STORE(model), &iter);
}

static void entityValueEdited (GtkCellRendererText *renderer, gchar *path, gchar* new_text, GtkTreeView *view)
{
	entityKeyValueEdited(view, 1, new_text);
}

static void entityKeyEdited (GtkCellRendererText *renderer, gchar *path, gchar* new_text, GtkTreeView *view)
{
	entityKeyValueEdited(view, 0, new_text);
}

/**
 * @brief callback invoked when key edit was canceled, used to remove newly added empty keys.
 * @param renderer cell renderer used to edit
 * @param view treeview that is edited
 */
static void entityKeyEditCanceled(GtkCellRendererText *renderer, GtkTreeView *view)
{
	char *oldKey;

	g_object_get(G_OBJECT(renderer), "text", &oldKey, (char*)0);
	StringOutputStream keyConverted(64);
	keyConverted << ConvertUTF8ToLocale(oldKey);
	if (keyConverted.empty()) {
		GtkTreeModel* model;
		GtkTreeIter iter;
		/* retrieve current selection and iter*/
		GtkTreeSelection *selection = gtk_tree_view_get_selection(view);
		gtk_tree_selection_get_selected(selection, &model, &iter);
		gtk_tree_store_remove(GTK_TREE_STORE(model), &iter);
	}
}

/**
 * @brief Callback for selection changed in entity key value list used to enable/disable buttons
 * @param selection current selection
 */
static void EntityKeyValueList_selection_changed (GtkTreeSelection* selection)
{
	ASSERT_NOTNULL(g_entityInspectorGui.m_btnRemoveKey);
	if (gtk_tree_selection_count_selected_rows(selection) == 0) {
		g_object_set(g_entityInspectorGui.m_btnRemoveKey, "sensitive", FALSE, (const char*) 0);
		g_object_set(g_entityInspectorGui.m_btnAddKey, "sensitive", FALSE, (const char*) 0);
	} else {
		GtkTreeModel *model;
		GtkTreeIter iter;
		GtkTreeIter parent;
		bool removeAllowed = true;
		char* attribKey;
		char* classname;
		gtk_tree_selection_get_selected(selection, &model, &iter);
		if (gtk_tree_model_iter_parent(model, &parent, &iter) == FALSE)
		{
			// no parent -> top level = classname selected, may not be removed
			removeAllowed = false;
			gtk_tree_model_get(model, &iter, KEYVALLIST_COLUMN_VALUE, &classname, -1);
		} else {
			gtk_tree_model_get(model, &iter, KEYVALLIST_COLUMN_KEY, &attribKey, -1);
			gtk_tree_model_get(model, &parent, KEYVALLIST_COLUMN_VALUE, &classname, -1);
			ASSERT_NOTNULL(attribKey);
			if (strlen(attribKey) == 0) {
				/* new attribute, don't check for remove yet */
				removeAllowed = false;
			}
		}
		/* check whether attributes reflect current keyvalue list selected class */
		if (strcmp(g_current_attributes->name(),classname)) {
			/* entity class does not fit the selection from keyvaluelist, update as needed */
			EntityClass *eclass = GlobalEntityClassManager().findOrInsert(classname, false);
			EntityInspector_setEntityClass(eclass);
		}
		if (attribKey != 0 && strlen(attribKey) > 0) {
			g_currentSelectedKey = CopiedString(attribKey);
		}
		if (removeAllowed) {
			EntityClassAttribute *attribute = g_current_attributes->getAttribute(attribKey);
			ASSERT_NOTNULL(attribute);
			if (attribute->m_mandatory)
				removeAllowed = false;
		}
		g_object_set(g_entityInspectorGui.m_btnRemoveKey, "sensitive", removeAllowed, (const char*) 0);
		g_object_set(g_entityInspectorGui.m_btnAddKey, "sensitive", (g_numNewKeys > 0), (const char*) 0);
	}
}

/**
 * @brief Callback for "editing-started" signal for key column used to update combo renderer with appropriate values.
 *
 * @param renderer combo renderer used for editing
 * @param editable unused
 * @param path unused
 * @param user_data unused
 */
static void EntityKeyValueList_keyEditingStarted (GtkCellRenderer *renderer,
		GtkCellEditable *editable, const gchar *path , gpointer *user_data)
{
	if (!g_current_attributes)
		return;
	/* determine available key values and set them to combo box */
	GtkListStore* store;
	g_object_get(renderer, "model", &store, (char*)0);
	gtk_list_store_clear(store);

	GtkTreeIter storeIter;
	const EntityClassAttributes validAttrib = g_current_attributes->m_attributes;
	for (EntityClassAttributes::const_iterator i = validAttrib.begin(); i != validAttrib.end(); ++i) {
		EntityClassAttribute *attrib = const_cast<EntityClassAttribute*> (&(*i).second);
		ClassKeyValues::iterator it = g_selectedKeyValues.find(g_current_attributes->m_name);
		if (it == g_selectedKeyValues.end()) {
			return;
		}
		KeyValues possibleValues = (*it).second;
		KeyValues::const_iterator keyIter = possibleValues.find(attrib->m_type.c_str());
		/* end means we don't have it actually in this map, so this is a valid new key*/
		if (keyIter == possibleValues.end()) {
			gtk_list_store_append(store, &storeIter);
			gtk_list_store_set(store, &storeIter, 0, attrib->m_type.c_str(), -1);
		}
	}
}

/**
 * @brief Callback for "editing-started" signal for value column used to update combo renderer with appropriate values.
 *
 * @param renderer combo renderer used for editing
 * @param editable unused
 * @param path unused
 * @param user_data unused
 */
static void EntityKeyValueList_valueEditingStarted (GtkCellRenderer *renderer,
		GtkCellEditable *editable, const gchar *path , gpointer *user_data)
{
	// prevent update if already displaying anything
	{
		bool editing;
		g_object_get(G_OBJECT(renderer), "editing", &editing, (const char*)0);
		if (editing)
			return;
	}
	if (g_currentSelectedKey.empty()) {
		g_warning("leaving updateValueCombo... no g_currentSelectedKey");
		return;
	}
	const char *key = g_currentSelectedKey.c_str();
	ClassKeyValues::iterator it = g_selectedKeyValues.find(g_current_attributes->m_name);
	if (it == g_selectedKeyValues.end()) {
		g_warning("leaving updateValueCombo... no class attributes");
		return;
	}
	KeyValues possibleValues = (*it).second;
	KeyValues::const_iterator keyIter = possibleValues.find(key);
	GtkListStore* store;
	g_object_get(renderer, "model", &store, (char*)0);
	gtk_list_store_clear(store);
	GtkTreeIter storeIter;
	// end means we don't have it actually in this map, so this is a valid new key
	if (keyIter != possibleValues.end()) {
		Values values = (*keyIter).second;
		for (Values::const_iterator valueIter = values.begin(); valueIter != values.end(); valueIter++) {
			gtk_list_store_append(store, &storeIter);
			gtk_list_store_set(store, &storeIter, 0, (*valueIter).c_str(), -1);
		}
	} else {
		gtk_list_store_append(store, &storeIter);
		gtk_list_store_set(store, &storeIter, 0, "", -1);
	}
}

GtkWidget* EntityInspector_constructNotebookTab (void)
{
	GtkWidget* pageframe = gtk_frame_new(_("Entity Inspector"));
	GtkWidget* vbox = gtk_vbox_new(FALSE, 2);

	gtk_container_set_border_width(GTK_CONTAINER(pageframe), 2);

	{
		gtk_container_add(GTK_CONTAINER(pageframe), vbox);

		{
			// entity class list
			GtkWidget* scr = gtk_scrolled_window_new(0, 0);
			gtk_box_pack_start(GTK_BOX(vbox), scr, TRUE, TRUE, 0);
			gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scr), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
			gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scr), GTK_SHADOW_IN);

			{
				GtkListStore* store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_POINTER);

				GtkTreeView* view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(GTK_TREE_MODEL(store)));
				gtk_tree_view_set_enable_search(GTK_TREE_VIEW(view), FALSE);
				gtk_tree_view_set_headers_visible(view, FALSE);
				g_signal_connect(G_OBJECT(view), "button_press_event", G_CALLBACK(EntityClassList_button_press), 0);
				g_signal_connect(G_OBJECT(view), "key_press_event", G_CALLBACK(EntityClassList_keypress), 0);

				{
					GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
					GtkTreeViewColumn* column = gtk_tree_view_column_new_with_attributes(_("Key"), renderer, "text",
							0, (char const*) 0);
					gtk_tree_view_append_column(view, column);
				}

				{
					GtkTreeSelection* selection = gtk_tree_view_get_selection(view);
					g_signal_connect(G_OBJECT(selection), "changed", G_CALLBACK(EntityClassList_selection_changed),
							0);
				}

				gtk_container_add(GTK_CONTAINER(scr), GTK_WIDGET(view));

				g_object_unref(G_OBJECT(store));
				g_entityClassList = view;
				g_entlist_store = store;
			}
		}

		{
			// entity class comments
			GtkWidget* scr = gtk_scrolled_window_new(0, 0);
			gtk_box_pack_start(GTK_BOX(vbox), scr, TRUE, TRUE, 0);
			gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scr), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
			gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scr), GTK_SHADOW_IN);

			{
				GtkTextView* text = GTK_TEXT_VIEW(gtk_text_view_new());
				widget_set_size(GTK_WIDGET(text), 0, 0); // as small as possible
				gtk_text_view_set_wrap_mode(text, GTK_WRAP_WORD);
				gtk_text_view_set_editable(text, FALSE);
				gtk_container_add(GTK_CONTAINER(scr), GTK_WIDGET(text));
				g_entityClassComment = text;
			}
		}

		{
			// Spawnflags (4 colums wide max, or window gets too wide.)
			GtkTable* table = GTK_TABLE(gtk_table_new(4, 4, FALSE));
			gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(table), FALSE, TRUE, 0);

			g_spawnflagsTable = table;

			for (int i = 0; i < MAX_FLAGS; i++) {
				GtkCheckButton* check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(""));
				gtk_widget_ref(GTK_WIDGET(check));
				g_object_set_data(G_OBJECT(check), "handler", gint_to_pointer(g_signal_connect(G_OBJECT(check),
						"toggled", G_CALLBACK(SpawnflagCheck_toggled), 0)));
				g_entitySpawnflagsCheck[i] = check;
			}
		}

		{
			// entity key/value list
			GtkWidget* scr = gtk_scrolled_window_new(0, 0);
			gtk_box_pack_start(GTK_BOX(vbox), scr, TRUE, TRUE, 0);
			gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scr), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
			gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scr), GTK_SHADOW_IN);

			GtkTreeStore* store = gtk_tree_store_new(KEYVALLIST_MAX_COLUMN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_BOOLEAN);

			GtkTreeView* view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(GTK_TREE_MODEL(store)));
			gtk_tree_view_set_enable_search(view, FALSE);
			gtk_tree_view_set_show_expanders(view, FALSE);
			gtk_tree_view_set_level_indentation(view, 10);
			/* expand all rows after the treeview widget has been realized */
			g_signal_connect(view, "realize", G_CALLBACK(gtk_tree_view_expand_all), NULL);
			{
				GtkCellRenderer* renderer = gtk_cell_renderer_combo_new();
				GtkTreeViewColumn* column = gtk_tree_view_column_new_with_attributes(_("Key"), renderer, "text",
						KEYVALLIST_COLUMN_KEY, (char const*) 0);
				gtk_tree_view_column_add_attribute(column, renderer, "weight", KEYVALLIST_COLUMN_STYLE);
				gtk_tree_view_column_add_attribute(column, renderer, "editable", KEYVALLIST_COLUMN_KEY_EDITABLE);
				{
					GtkListStore* rendererStore = gtk_list_store_new(1, G_TYPE_STRING);
					g_object_set(renderer, "model", GTK_TREE_MODEL(rendererStore), "text-column", 0, (const char*) 0);
					g_object_unref( G_OBJECT (rendererStore));
				}
				gtk_tree_view_append_column(view, column);
				/** @todo there seems to be a display problem with has-entries for worldspawn attributes using windows , check that */
				g_object_set(renderer, "editable", TRUE, "editable-set", TRUE, "has-entry", FALSE, (char const*) 0);
				g_signal_connect(G_OBJECT(renderer), "edited", G_CALLBACK(entityKeyEdited), (gpointer) view);
				g_signal_connect(G_OBJECT(renderer), "editing-canceled", G_CALLBACK(entityKeyEditCanceled), (gpointer) view);
				g_signal_connect(G_OBJECT(renderer), "editing-started", G_CALLBACK(EntityKeyValueList_keyEditingStarted), (gpointer) view);
			}

			{
				GtkCellRenderer* renderer = gtk_cell_renderer_combo_new();
				GtkTreeViewColumn* column = gtk_tree_view_column_new_with_attributes(_("Value"), renderer,
						"text", KEYVALLIST_COLUMN_VALUE, (char const*) 0);
				gtk_tree_view_column_add_attribute(column, renderer, "weight", KEYVALLIST_COLUMN_STYLE);
				gtk_tree_view_append_column(view, column);
				{
					GtkListStore* rendererStore = gtk_list_store_new(1, G_TYPE_STRING);
					g_object_set(renderer, "model", GTK_TREE_MODEL(rendererStore), "text-column", 0, (const char*) 0);
					g_object_unref( G_OBJECT (rendererStore));
				}
				g_object_set(renderer, "editable", TRUE, "editable-set", TRUE, (char const*) 0);
				g_signal_connect(G_OBJECT(renderer), "edited", G_CALLBACK(entityValueEdited), (gpointer) view);
				g_signal_connect(G_OBJECT(renderer), "editing-started", G_CALLBACK(EntityKeyValueList_valueEditingStarted), (gpointer) view);
			}

			{
				GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
				g_signal_connect(G_OBJECT(selection), "changed", G_CALLBACK(EntityKeyValueList_selection_changed), NULL);
			}
			g_entityInspectorGui.m_viewKeyValues = view;

			gtk_container_add(GTK_CONTAINER(scr), GTK_WIDGET(view));

			g_object_unref(G_OBJECT(store));

			g_entprops_store = store;

			// entity parameter action buttons
			GtkBox* hbox = GTK_BOX(gtk_hbox_new(TRUE, 4));
			gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(hbox), FALSE, TRUE, 0);

			{
				GtkButton* button = GTK_BUTTON(gtk_button_new_from_stock(GTK_STOCK_REMOVE));
				g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(EntityInspector_clearKeyValue),
						gpointer(view));
				gtk_box_pack_start(hbox, GTK_WIDGET(button), TRUE, TRUE, 0);
				g_object_set(button, "sensitive", FALSE, (const char*) 0);
				g_entityInspectorGui.m_btnRemoveKey = button;
			}
			{
				GtkButton* button = GTK_BUTTON(gtk_button_new_from_stock(GTK_STOCK_NEW));
				g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(EntityInspector_addKeyValue),
						gpointer(view));
				gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(button), TRUE, TRUE, 0);
				g_object_set(button, "sensitive", FALSE, (const char*) 0);
				g_entityInspectorGui.m_btnAddKey = button;
			}
		}
		{
			g_attributeBox = GTK_VBOX(gtk_vbox_new(FALSE, 2));
			gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g_attributeBox), FALSE, FALSE, 0);
		}
	}

	EntityClassList_fill();

	typedef FreeCaller1<const Selectable&, EntityInspector_selectionChanged> EntityInspectorSelectionChangedCaller;
	GlobalSelectionSystem().addSelectionChangeCallback(EntityInspectorSelectionChangedCaller());
	GlobalEntityCreator().setKeyValueChangedFunc(EntityInspector_keyValueChanged);

	return pageframe;
}

/// todo remove me?
class EntityInspector: public ModuleObserver
{
		std::size_t m_unrealised;
	public:
		EntityInspector () :
			m_unrealised(1)
		{
		}
		void realise (void)
		{
			if (--m_unrealised == 0) {
				EntityClassList_fill();
			}
		}
		void unrealise (void)
		{
			if (++m_unrealised == 1) {
				EntityClassList_clear();
			}
		}
};

EntityInspector g_EntityInspector;

#include "preferencesystem.h"
#include "stringio.h"

void EntityInspector_construct (void)
{
	GlobalEntityClassManager().attach(g_EntityInspector);
}

void EntityInspector_destroy (void)
{
	GlobalEntityClassManager().detach(g_EntityInspector);
}
