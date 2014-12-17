# -*- Mode: makefile-gmake; tab-width: 4; indent-tabs-mode: t -*-
#
# This file is part of the LibreOffice project.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#

$(eval $(call gb_CustomTarget_CustomTarget,oox/generated))

oox_SRC := $(SRCDIR)/oox/source/token
oox_MISC := $(call gb_CustomTarget_get_workdir,oox/generated)/misc
oox_INC := $(call gb_CustomTarget_get_workdir,oox/generated)
oox_GENHEADERPATH := $(oox_INC)/oox/token

$(oox_MISC)/vml-shape-types : \
		$(SRCDIR)/oox/source/export/preset-definitions-to-shape-types.pl \
		$(SRCDIR)/oox/source/drawingml/customshapes/presetShapeDefinitions.xml \
		$(SRCDIR)/oox/source/export/presetTextWarpDefinitions.xml \
		$(SRCDIR)/oox/CustomTarget_generated.mk
	$(call gb_Output_announce,$(subst $(WORKDIR)/,,$@),build,PRL,1)
	mkdir -p $(dir $@)
	perl $< --vml-shape-types-data $(filter-out $<,$^) > $@.in_progress 2> $@.log && mv $@.in_progress $@

$(oox_MISC)/oox-drawingml-adj-names : \
		$(SRCDIR)/oox/source/export/preset-definitions-to-shape-types.pl \
		$(SRCDIR)/oox/source/drawingml/customshapes/presetShapeDefinitions.xml \
		$(SRCDIR)/oox/source/export/presetTextWarpDefinitions.xml \
		$(SRCDIR)/oox/CustomTarget_generated.mk
	$(call gb_Output_announce,$(subst $(WORKDIR)/,,$@),build,PRL,1)
	mkdir -p $(dir $@)
	perl $< --drawingml-adj-names-data $(filter-out $<,$^) > $@.in_progress 2> $@.log && mv $@.in_progress $@

# Generate tokens for oox
$(eval $(call gb_CustomTarget_token_hash,oox,tokenhash.inc,tokenhash.gperf))

$(eval $(call gb_CustomTarget_generate_tokens,oox,namespaces,namespace,namespaces.txt,namespaces-strict,namespaces.pl))
$(eval $(call gb_CustomTarget_generate_tokens,oox,properties,property,,,properties.pl))
$(eval $(call gb_CustomTarget_generate_tokens,oox,tokens,token,tokenhash.gperf))

$(call gb_CustomTarget_get_target,oox/generated) : \
	$(oox_MISC)/oox-drawingml-adj-names \
	$(oox_MISC)/vml-shape-types \
	$(oox_INC)/tokenhash.inc \
	$(oox_INC)/tokennames.inc \
	$(oox_INC)/namespacenames.inc \
	$(oox_INC)/namespaces-strictnames.inc \
	$(oox_INC)/propertynames.inc \
	$(oox_GENHEADERPATH)/tokens.hxx \
	$(oox_GENHEADERPATH)/namespaces.hxx \
	$(oox_GENHEADERPATH)/properties.hxx \
	$(oox_MISC)/namespaces.txt \

# vim: set noet sw=4 ts=4:
