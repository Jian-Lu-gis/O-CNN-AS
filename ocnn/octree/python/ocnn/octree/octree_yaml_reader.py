"""Module to parse OctreeDatasetParameters from config files"""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import yaml

from ocnn.octree.octree_settings import OctreeSettings
from ocnn.octree.octree_augmentor import OctreeAugmentorCollection


def _extract_augmentor_param(augmentor_yaml):
    """ Takes augmentor yaml dictionary and determines name and parameters
    Args:
      augmentor_yaml: Parameter dictionary containing augmentor data.
    """

    if isinstance(augmentor_yaml, str):
        augmentor_name = augmentor_yaml
        augmentor_param = None
    elif isinstance(augmentor_yaml, dict):
        assert len(augmentor_yaml) == 1
        augmentor_name, augmentor_param = next(iter(augmentor_yaml.items()))
    else:
        raise RuntimeError(
            'Augmentor parameters of type {0} not supported'.format(
                type(augmentor_yaml)))
    return augmentor_name, augmentor_param

class OctreeYamlReader:
    """ Creates dataset parameters from yaml"""
    @property
    def octree_settings(self):
        """ Octree parameters for OctreeProcessor"""
        return self._octree_settings

    @property
    def augmentor_collection(self):
        """ Augmentor collection for OctreeProcessor """
        return self._augmentor_collection

    def __init__(self, yml_filepath):
        """ Initializes OctreeDatasetParameters
        Args:
          yml_filepath: Path to yml config file.
        """
        with open(yml_filepath, 'r') as yml_file:
            config = yaml.load(yml_file)

        #Octree Settings
        parameters = config['octree_settings']
        self._octree_settings = OctreeSettings.from_dict(parameters)

        #Augmentor Collection
        if 'augmentation' in config:
            parameters = config['augmentation']
            self._augmentor_collection = self._create_augmentor_collection(
                augmentor_collection_yaml=parameters['augmentors'],
                total_aug=parameters['total_aug'])
        else:
            self._augmentor_collection = None

    def _create_augmentor_collection(self, augmentor_collection_yaml, total_aug):
        augmentor_collection = OctreeAugmentorCollection(total_aug)

        for augmentor_yaml in augmentor_collection_yaml:
            augmentor_name, augmentor_param = _extract_augmentor_param(augmentor_yaml)
            if augmentor_name == 'centering':
                augmentor_collection.add_centering_augmentor()
            elif augmentor_name == 'displacing':
                augmentor_collection.add_displacing_augmentor(
                    displacement=augmentor_param['displacement'],
                    depth=self.octree_settings.depth)
            elif augmentor_name == 'rotation':
                augmentor_collection.add_rotation_augmentor()
            elif augmentor_name == 'axial_rotation':
                augmentor_collection.add_axial_rotation_augmentor(
                    axis=augmentor_param['axis'])
            else:
                raise RuntimeError('Unknown augmentor {0}'.format(augmentor_name))
        return augmentor_collection
